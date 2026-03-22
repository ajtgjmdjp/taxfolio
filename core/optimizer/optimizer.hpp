#pragma once

#include "core/models/types.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openvolt {

/// Per-asset weight bounds.
struct WeightBound {
    double lo = 0.0;
    double hi = 1.0;
};

/// Parameters for the portfolio optimization problem.
struct OptimizationParams {
    // Objective weights
    double lambda_drift    = 200.0;   // Allocation drift penalty
    double lambda_tcost = 0.0;     // Transaction cost penalty
    double lambda_tax   = 400.0;   // Tax (realized gains) penalty

    // Constraints
    double weight_cap   = 0.05;    // Default max weight per asset
    double turnover_cap = 1.0;     // Max one-way turnover (100%)
    double invest_fraction = 1.0;  // sum(w) target (1.0 - cash_buffer_fraction)

    // Per-asset overrides (index → bounds)
    std::unordered_map<int, WeightBound> per_asset_bounds;
    // No-buy / no-sell sets (asset indices)
    std::unordered_set<int> no_buy;
    std::unordered_set<int> no_sell;

    // Per-asset transaction cost fractions (bps / 10000)
    Vector tcost_frac;  // length N, or empty for uniform

    // Tax parameters
    double tax_rate     = 0.20315; // Japan: 20.315%

    // Alpha (expected return) — optional, empty = no views
    Vector alpha;               // length N, or empty for no alpha term
    double lambda_alpha = 0.0;  // Alpha term weight (0 = disabled)
};

/// Result of a single optimization solve.
struct OptimizationResult {
    Vector target_weights;         // Optimal portfolio weights
    double objective_value;        // Optimal objective value
    double predicted_te;           // Ex-ante tracking error
    double predicted_turnover;     // Predicted one-way turnover
    bool converged;                // Did the solver converge?
    std::string solver_status;     // Solver status message
};

/// Abstract optimizer interface.
///
/// Given benchmark weights, current weights, a covariance matrix,
/// and optimization parameters, find optimal target weights.
class Optimizer {
public:
    virtual ~Optimizer() = default;

    /// Solve the portfolio optimization problem.
    /// @param benchmark_weights  Target benchmark weights (length N)
    /// @param current_weights    Current portfolio weights (length N)
    /// @param cov                N x N covariance matrix
    /// @param unrealized_gains   Per-asset unrealized gains (length N, for tax penalty)
    /// @param params             Optimization parameters
    /// @return Optimization result with target weights
    [[nodiscard]] virtual OptimizationResult solve(
        const Vector& benchmark_weights,
        const Vector& current_weights,
        const Matrix& cov,
        const Vector& unrealized_gains,
        const OptimizationParams& params
    ) const = 0;

    [[nodiscard]] virtual std::string name() const = 0;
};

/// Factory: create an optimizer by name.
/// @param name  "osqp" (default) or "scs"
[[nodiscard]] std::unique_ptr<Optimizer> make_optimizer(const std::string& name = "osqp");

} // namespace openvolt
