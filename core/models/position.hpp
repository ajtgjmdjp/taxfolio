#pragma once

#include "core/models/types.hpp"
#include <unordered_map>

namespace openvolt {

/// A single holding in a portfolio.
struct Position {
    Ticker ticker;
    Shares shares;
    Money cost_basis;  // Total acquisition cost (for tax calculations)

    /// Average cost per share.
    [[nodiscard]] Price avg_cost() const noexcept {
        return shares > 0.0 ? cost_basis / shares : 0.0;
    }

    /// Market value at a given price.
    [[nodiscard]] Money market_value(Price price) const noexcept {
        return shares * price;
    }

    /// Unrealized P&L at a given price.
    [[nodiscard]] Money unrealized_pnl(Price price) const noexcept {
        return market_value(price) - cost_basis;
    }

    /// Unrealized P&L as a fraction of cost basis.
    [[nodiscard]] double unrealized_pnl_pct(Price price) const noexcept {
        return cost_basis > 0.0 ? unrealized_pnl(price) / cost_basis : 0.0;
    }
};

/// A collection of positions keyed by ticker.
using Holdings = std::unordered_map<Ticker, Position>;

} // namespace openvolt
