#pragma once

#include "core/models/types.hpp"
#include <string>

namespace openvolt {

/// Reason for a trade.
enum class TradeReason {
    Rebalance,
    TaxLossHarvest,
    EmergencyRebalance,
    CashFlow,
    Initial,
};

/// Convert TradeReason to string.
[[nodiscard]] constexpr const char* to_string(TradeReason reason) noexcept {
    switch (reason) {
        case TradeReason::Rebalance:          return "REBALANCE";
        case TradeReason::TaxLossHarvest:     return "TLH";
        case TradeReason::EmergencyRebalance: return "EMERGENCY";
        case TradeReason::CashFlow:           return "CASHFLOW";
        case TradeReason::Initial:            return "INITIAL";
    }
    return "UNKNOWN";
}

/// Side of a trade.
enum class Side { Buy, Sell };

/// A single executed trade.
struct Trade {
    Date date;
    Ticker ticker;
    Side side;
    Shares shares;       // Always positive
    Price price;
    Money amount;         // shares * price
    Money cost_basis;     // For sells: cost basis of sold shares
    Money realized_pnl;   // For sells: amount - cost_basis
    TradeReason reason;
};

} // namespace openvolt
