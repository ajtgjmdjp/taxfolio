#pragma once

#include "core/models/types.hpp"
#include <vector>
#include <algorithm>

namespace openvolt {

/// A single tax lot — represents shares acquired at a specific time and price.
/// Tax lots are the fundamental unit for computing realized gains/losses.
struct TaxLot {
    Date acquisition_date;
    Shares shares;
    Price cost_per_share;

    /// Total cost basis of this lot.
    [[nodiscard]] Money cost_basis() const noexcept {
        return shares * cost_per_share;
    }

    /// Unrealized P&L at a given market price.
    [[nodiscard]] Money unrealized_pnl(Price market_price) const noexcept {
        return shares * (market_price - cost_per_share);
    }

    /// Unrealized P&L as a percentage of cost.
    [[nodiscard]] double unrealized_pnl_pct(Price market_price) const noexcept {
        return cost_per_share > 0.0
            ? (market_price - cost_per_share) / cost_per_share
            : 0.0;
    }
};

/// Tax lot disposition method.
enum class LotDisposition {
    FIFO,                 // First In, First Out
    LIFO,                 // Last In, First Out
    HighestCost,          // Sell highest cost lots first (minimize gains)
    LowestCost,           // Sell lowest cost lots first
    TaxOptimal,           // Sell lots to minimize tax impact
    SpecificIdentification // Caller specifies which lots
};

/// Collection of tax lots for a single ticker.
class TaxLotCollection {
public:
    /// Add a new lot (purchase).
    void add_lot(const TaxLot& lot) {
        lots_.push_back(lot);
    }

    /// Sell shares using the specified disposition method.
    /// Returns: total cost basis of sold shares (for realized P&L calculation).
    /// Modifies the lot collection in-place.
    [[nodiscard]] Money sell(
        Shares shares_to_sell,
        Price sale_price,
        LotDisposition method = LotDisposition::TaxOptimal
    ) {
        // Sort lots according to disposition method
        sort_lots(method, sale_price);

        Money total_cost_basis = 0.0;
        Shares remaining = shares_to_sell;

        for (auto it = lots_.begin(); it != lots_.end() && remaining > 1e-10; ) {
            if (it->shares <= remaining) {
                // Sell entire lot
                total_cost_basis += it->cost_basis();
                remaining -= it->shares;
                it = lots_.erase(it);
            } else {
                // Partial sell
                total_cost_basis += remaining * it->cost_per_share;
                it->shares -= remaining;
                remaining = 0.0;
                ++it;
            }
        }

        return total_cost_basis;
    }

    /// Total shares across all lots.
    [[nodiscard]] Shares total_shares() const noexcept {
        Shares total = 0.0;
        for (const auto& lot : lots_) {
            total += lot.shares;
        }
        return total;
    }

    /// Total cost basis across all lots.
    [[nodiscard]] Money total_cost_basis() const noexcept {
        Money total = 0.0;
        for (const auto& lot : lots_) {
            total += lot.cost_basis();
        }
        return total;
    }

    /// Total unrealized P&L at a given price.
    [[nodiscard]] Money total_unrealized_pnl(Price market_price) const noexcept {
        Money total = 0.0;
        for (const auto& lot : lots_) {
            total += lot.unrealized_pnl(market_price);
        }
        return total;
    }

    /// Number of lots.
    [[nodiscard]] size_t lot_count() const noexcept { return lots_.size(); }

    /// Access lots (read-only).
    [[nodiscard]] const std::vector<TaxLot>& lots() const noexcept { return lots_; }

    /// Check if any lot has a loss (for TLH candidate detection).
    [[nodiscard]] bool has_loss(Price market_price) const noexcept {
        return std::any_of(lots_.begin(), lots_.end(),
            [market_price](const TaxLot& lot) {
                return lot.unrealized_pnl(market_price) < 0.0;
            });
    }

    /// Sum of unrealized losses only (for TLH harvesting potential).
    [[nodiscard]] Money harvestable_loss(Price market_price) const noexcept {
        Money loss = 0.0;
        for (const auto& lot : lots_) {
            const Money pnl = lot.unrealized_pnl(market_price);
            if (pnl < 0.0) {
                loss += pnl;  // Negative value
            }
        }
        return loss;
    }

private:
    std::vector<TaxLot> lots_;

    void sort_lots(LotDisposition method, Price market_price) {
        switch (method) {
            case LotDisposition::FIFO:
                std::sort(lots_.begin(), lots_.end(),
                    [](const TaxLot& a, const TaxLot& b) {
                        return a.acquisition_date < b.acquisition_date;
                    });
                break;
            case LotDisposition::LIFO:
                std::sort(lots_.begin(), lots_.end(),
                    [](const TaxLot& a, const TaxLot& b) {
                        return a.acquisition_date > b.acquisition_date;
                    });
                break;
            case LotDisposition::HighestCost:
                std::sort(lots_.begin(), lots_.end(),
                    [](const TaxLot& a, const TaxLot& b) {
                        return a.cost_per_share > b.cost_per_share;
                    });
                break;
            case LotDisposition::LowestCost:
                std::sort(lots_.begin(), lots_.end(),
                    [](const TaxLot& a, const TaxLot& b) {
                        return a.cost_per_share < b.cost_per_share;
                    });
                break;
            case LotDisposition::TaxOptimal:
                // Sell lots with losses first (harvest losses),
                // then highest cost lots (minimize gains).
                std::sort(lots_.begin(), lots_.end(),
                    [market_price](const TaxLot& a, const TaxLot& b) {
                        const double pnl_a = a.unrealized_pnl_pct(market_price);
                        const double pnl_b = b.unrealized_pnl_pct(market_price);
                        // Losses first (most negative first),
                        // then among gains, highest cost first
                        if (pnl_a < 0.0 && pnl_b >= 0.0) return true;
                        if (pnl_a >= 0.0 && pnl_b < 0.0) return false;
                        if (pnl_a < 0.0 && pnl_b < 0.0) return pnl_a < pnl_b;
                        return a.cost_per_share > b.cost_per_share;
                    });
                break;
            case LotDisposition::SpecificIdentification:
                // No sorting — caller controls order
                break;
        }
    }
};

} // namespace openvolt
