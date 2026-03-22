#include "core/models/tax_policy.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openvolt {

// ---------------------------------------------------------------------------
// JapanTaxPolicy
// ---------------------------------------------------------------------------

std::vector<TaxCostEstimate> JapanTaxPolicy::estimate_tax_costs(
    const std::unordered_map<Ticker, TaxLotCollection>& lots_by_ticker,
    const std::unordered_map<Ticker, Price>& prices,
    double total_portfolio_value,
    Date /*as_of_date*/
) const {
    std::vector<TaxCostEstimate> estimates;

    for (const auto& [ticker, lots] : lots_by_ticker) {
        auto pit = prices.find(ticker);
        if (pit == prices.end()) continue;

        Price price = pit->second;
        Shares total_shares = lots.total_shares();
        if (total_shares <= 0.0) continue;

        // Average cost method (移動平均法)
        double avg_cost = lots.total_cost_basis() / total_shares;
        double gain_per_share = price - avg_cost;

        // Tax cost per unit of portfolio weight sold
        double position_value = total_shares * price;
        double weight = position_value / total_portfolio_value;

        double cost_per_unit = 0.0;
        if (weight > 0.0) {
            // Positive gain = tax cost, negative gain = tax benefit (incentive to sell)
            cost_per_unit = gain_per_share * total_shares * tax_rate_ / total_portfolio_value;
        }

        estimates.push_back({ticker, cost_per_unit});
    }

    return estimates;
}

LotDispositionResult JapanTaxPolicy::dispose_lots(
    TaxLotCollection& lots,
    Shares shares_to_sell,
    Price sale_price,
    Date sale_date
) const {
    LotDispositionResult result;
    result.total_tax = 0.0;

    // Japan 特定口座: average cost method
    // All lots are treated equally — use average cost basis
    Shares total_shares = lots.total_shares();
    if (total_shares <= 0.0 || shares_to_sell <= 0.0) return result;

    double avg_cost = lots.total_cost_basis() / total_shares;
    Money cost_basis = shares_to_sell * avg_cost;
    Money proceeds = shares_to_sell * sale_price;
    Money gain = proceeds - cost_basis;
    Money tax = gain > 0.0 ? gain * tax_rate_ : 0.0;

    // Actually remove shares from lots (FIFO order for bookkeeping)
    Money actual_cost = lots.sell(shares_to_sell, sale_price, LotDisposition::FIFO);

    result.entries.push_back({
        0,  // lot_id 0 = averaged
        "",  // ticker set by caller
        shares_to_sell,
        proceeds,
        cost_basis,
        gain,
        false,  // Japan has no long-term distinction
        tax,
    });
    result.total_tax = tax;

    return result;
}

// ---------------------------------------------------------------------------
// USTaxPolicy
// ---------------------------------------------------------------------------

std::vector<TaxCostEstimate> USTaxPolicy::estimate_tax_costs(
    const std::unordered_map<Ticker, TaxLotCollection>& lots_by_ticker,
    const std::unordered_map<Ticker, Price>& prices,
    double total_portfolio_value,
    Date as_of_date
) const {
    std::vector<TaxCostEstimate> estimates;

    for (const auto& [ticker, lots] : lots_by_ticker) {
        auto pit = prices.find(ticker);
        if (pit == prices.end()) continue;

        Price price = pit->second;

        // Boyd-lite: compute LTFO-ordered marginal tax cost per asset.
        // Each lot's tax rate depends on its holding period (ST vs LT).
        double tax_cost = 0.0;
        for (const auto& lot : lots.lots()) {
            double gain = lot.shares * (price - lot.cost_per_share);
            // Use actual holding period for ST/LT determination
            double rate;
            if (as_of_date != Date{}) {
                rate = tax_rate_for_lot(lot.acquisition_date, as_of_date);
            } else {
                rate = short_term_rate_;  // Conservative fallback
            }

            // Both gains and losses are tax-relevant:
            // gain > 0 → selling incurs tax at rate
            // gain < 0 → selling harvests loss (negative tax = benefit)
            tax_cost += gain * rate;
        }

        double cost_per_unit = tax_cost / total_portfolio_value;
        estimates.push_back({ticker, cost_per_unit});
    }

    return estimates;
}

LotDispositionResult USTaxPolicy::dispose_lots(
    TaxLotCollection& lots,
    Shares shares_to_sell,
    Price sale_price,
    Date sale_date
) const {
    LotDispositionResult result;
    result.total_tax = 0.0;

    // Tax-optimal lot selection: losses first, then highest cost
    // Sort: losses first (most negative PnL), then among gains highest cost first
    auto& lot_vec = const_cast<std::vector<TaxLot>&>(lots.lots());

    std::vector<std::pair<size_t, double>> scored;
    for (size_t i = 0; i < lot_vec.size(); ++i) {
        double pnl = lot_vec[i].unrealized_pnl_pct(sale_price);
        scored.push_back({i, pnl});
    }

    // Sort: most negative first, then highest cost
    std::sort(scored.begin(), scored.end(),
        [&](const auto& a, const auto& b) {
            if (a.second < 0.0 && b.second >= 0.0) return true;
            if (a.second >= 0.0 && b.second < 0.0) return false;
            if (a.second < 0.0 && b.second < 0.0) return a.second < b.second;
            return lot_vec[a.first].cost_per_share > lot_vec[b.first].cost_per_share;
        });

    Shares remaining = shares_to_sell;

    for (const auto& [idx, _] : scored) {
        if (remaining <= 1e-10) break;
        auto& lot = lot_vec[idx];

        Shares sell_shares = std::min(remaining, lot.shares);
        Money proceeds = sell_shares * sale_price;
        Money cost = sell_shares * lot.cost_per_share;
        Money gain = proceeds - cost;
        bool lt = is_long_term(lot.acquisition_date, sale_date);
        double rate = lt ? long_term_rate_ : short_term_rate_;
        Money tax = gain > 0.0 ? gain * rate : 0.0;

        result.entries.push_back({
            static_cast<uint64_t>(idx + 1),
            "",  // ticker set by caller
            sell_shares,
            proceeds,
            cost,
            gain,
            lt,
            tax,
        });
        result.total_tax += tax;

        lot.shares -= sell_shares;
        remaining -= sell_shares;
    }

    // Remove empty lots
    lot_vec.erase(
        std::remove_if(lot_vec.begin(), lot_vec.end(),
            [](const TaxLot& l) { return l.shares <= 1e-10; }),
        lot_vec.end()
    );

    return result;
}

WashSaleCheck USTaxPolicy::check_wash_sale(
    const Ticker& ticker,
    Date purchase_date,
    const std::vector<LotDispositionResult::Entry>& recent_sales
) const {
    // Check if any recent sale of the same ticker within the wash sale window
    // had a realized loss
    for (const auto& sale : recent_sales) {
        if (sale.ticker != ticker) continue;
        if (sale.realized_gain >= 0.0) continue;  // Not a loss

        // The wash sale window is 30 days before and after the sale
        // For simplicity, we check if the purchase is within wash_sale_window_days
        // of any loss-generating sale
        // Note: we don't have the sale date in the Entry, so this is approximate
        // In production, Entry should include the sale date

        return {
            true,
            sale.lot_id,
            -sale.realized_gain,  // Positive value = disallowed loss amount
        };
    }

    return {false, std::nullopt, 0.0};
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<ITaxPolicy> make_tax_policy(
    const std::string& jurisdiction,
    double short_term_rate,
    double long_term_rate,
    int wash_sale_window_days
) {
    if (jurisdiction == "japan" || jurisdiction == "jp") {
        return std::make_unique<JapanTaxPolicy>(short_term_rate);
    } else if (jurisdiction == "us") {
        return std::make_unique<USTaxPolicy>(short_term_rate, long_term_rate, wash_sale_window_days);
    }
    throw std::invalid_argument("Unknown tax jurisdiction: " + jurisdiction);
}

} // namespace openvolt
