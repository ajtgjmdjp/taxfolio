#pragma once

#include "core/models/position.hpp"
#include "core/models/trade.hpp"
#include <vector>

namespace openvolt {

/// Snapshot of portfolio state at a point in time.
struct PortfolioSnapshot {
    Date date;
    Money nav;              // Net Asset Value
    Money cash;
    double total_return;    // Cumulative return from inception
    double daily_return;
    double tracking_error;  // Ex-post TE (rolling, annualized)
};

/// Complete result of a simulation run.
struct SimulationResult {
    std::vector<PortfolioSnapshot> daily_snapshots;
    std::vector<Trade> trade_history;

    // Summary statistics
    double annualized_return;
    double annualized_volatility;
    double sharpe_ratio;
    double max_drawdown;
    double annualized_tracking_error;
    double information_ratio;
    Money total_realized_pnl;
    Money total_tax_paid;
    int total_trades;
    double turnover;  // Annual average
};

} // namespace openvolt
