#pragma once

#include "core/models/types.hpp"
#include "core/models/tax_lot.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace openvolt {

/// Per-asset tax cost estimate for the optimizer.
/// Positive = selling this asset incurs tax. Negative = selling harvests a loss.
struct TaxCostEstimate {
    Ticker ticker;
    double cost_per_unit;  // Tax cost per unit of weight sold
};

/// Result of disposing lots for a single sell trade.
struct LotDispositionResult {
    struct Entry {
        uint64_t lot_id;
        Ticker ticker;
        Shares shares_sold;
        Money proceeds;
        Money cost_basis;
        Money realized_gain;
        bool is_long_term;
        Money tax_liability;
    };
    std::vector<Entry> entries;
    Money total_tax;
};

/// Wash sale check result.
struct WashSaleCheck {
    bool is_wash_sale;
    std::optional<uint64_t> disallowed_lot_id;
    Money disallowed_loss;
};

/// Abstract tax policy interface.
/// Different jurisdictions implement different rules for:
/// - How gains are classified (short-term vs long-term)
/// - How lots are disposed (specific ID, FIFO, LIFO, average cost)
/// - Wash sale rules
/// - Tax rates
class ITaxPolicy {
public:
    virtual ~ITaxPolicy() = default;

    /// Human-readable name of this policy.
    [[nodiscard]] virtual std::string name() const = 0;

    /// Estimate tax cost per asset for the optimizer objective function.
    /// This is called BEFORE optimization to inform the QP.
    /// as_of_date is used for ST/LT determination.
    [[nodiscard]] virtual std::vector<TaxCostEstimate> estimate_tax_costs(
        const std::unordered_map<Ticker, TaxLotCollection>& lots_by_ticker,
        const std::unordered_map<Ticker, Price>& prices,
        double total_portfolio_value,
        Date as_of_date
    ) const = 0;

    /// Dispose lots for a sell trade. Called AFTER optimization.
    [[nodiscard]] virtual LotDispositionResult dispose_lots(
        TaxLotCollection& lots,
        Shares shares_to_sell,
        Price sale_price,
        Date sale_date
    ) const = 0;

    /// Check if a purchase would trigger a wash sale.
    [[nodiscard]] virtual WashSaleCheck check_wash_sale(
        const Ticker& ticker,
        Date purchase_date,
        const std::vector<LotDispositionResult::Entry>& recent_sales
    ) const = 0;
};

// ---------------------------------------------------------------------------
// Japan tax policy
// ---------------------------------------------------------------------------

/// Japan: flat 20.315% on all gains, no short/long distinction,
/// no wash sale rule, average cost method for 特定口座.
class JapanTaxPolicy final : public ITaxPolicy {
public:
    explicit JapanTaxPolicy(double tax_rate = 0.20315)
        : tax_rate_(tax_rate) {}

    [[nodiscard]] std::string name() const override { return "japan"; }

    [[nodiscard]] std::vector<TaxCostEstimate> estimate_tax_costs(
        const std::unordered_map<Ticker, TaxLotCollection>& lots_by_ticker,
        const std::unordered_map<Ticker, Price>& prices,
        double total_portfolio_value,
        Date as_of_date
    ) const override;

    [[nodiscard]] LotDispositionResult dispose_lots(
        TaxLotCollection& lots,
        Shares shares_to_sell,
        Price sale_price,
        Date sale_date
    ) const override;

    [[nodiscard]] WashSaleCheck check_wash_sale(
        const Ticker& ticker,
        Date purchase_date,
        const std::vector<LotDispositionResult::Entry>& recent_sales
    ) const override {
        // Japan has no wash sale rule
        return {false, std::nullopt, 0.0};
    }

private:
    double tax_rate_;
};

// ---------------------------------------------------------------------------
// US tax policy
// ---------------------------------------------------------------------------

/// US: short-term (< 1 year) vs long-term rates,
/// specific identification default, 30-day wash sale window.
class USTaxPolicy final : public ITaxPolicy {
public:
    USTaxPolicy(double short_term_rate = 0.37, double long_term_rate = 0.20,
                int wash_sale_window_days = 30)
        : short_term_rate_(short_term_rate),
          long_term_rate_(long_term_rate),
          wash_sale_window_days_(wash_sale_window_days) {}

    [[nodiscard]] std::string name() const override { return "us"; }

    [[nodiscard]] std::vector<TaxCostEstimate> estimate_tax_costs(
        const std::unordered_map<Ticker, TaxLotCollection>& lots_by_ticker,
        const std::unordered_map<Ticker, Price>& prices,
        double total_portfolio_value,
        Date as_of_date
    ) const override;

    [[nodiscard]] LotDispositionResult dispose_lots(
        TaxLotCollection& lots,
        Shares shares_to_sell,
        Price sale_price,
        Date sale_date
    ) const override;

    [[nodiscard]] WashSaleCheck check_wash_sale(
        const Ticker& ticker,
        Date purchase_date,
        const std::vector<LotDispositionResult::Entry>& recent_sales
    ) const override;

private:
    double short_term_rate_;
    double long_term_rate_;
    int wash_sale_window_days_;

    [[nodiscard]] bool is_long_term(Date acquired, Date sold) const {
        auto acq_days = std::chrono::sys_days{acquired};
        auto sold_days = std::chrono::sys_days{sold};
        return (sold_days - acq_days).count() > 365;
    }

    [[nodiscard]] double tax_rate_for_lot(Date acquired, Date sold) const {
        return is_long_term(acquired, sold) ? long_term_rate_ : short_term_rate_;
    }
};

/// Factory: create a tax policy by jurisdiction name.
[[nodiscard]] std::unique_ptr<ITaxPolicy> make_tax_policy(
    const std::string& jurisdiction,
    double short_term_rate = 0.37,
    double long_term_rate = 0.20,
    int wash_sale_window_days = 30
);

} // namespace openvolt
