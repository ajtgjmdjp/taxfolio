#pragma once

/// @file api.hpp
/// @brief OpenVolt public API — the single entry point for portfolio optimization.
///
/// This header defines the request/response types for plan_rebalance(),
/// the core function that turns portfolio state + market data + config
/// into an optimal trade list with tax lot dispositions.
///
/// Design principles:
/// - No Eigen types in the public API (plain STL containers at the boundary).
/// - Single request/response pair.
/// - Tax lot dispositions are always included (not optional).

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace ov {

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

using AssetId = std::string;
using LotId = std::uint64_t;
using Date = std::chrono::sys_days;

enum class Side { buy, sell };

enum class DisposalMethod {
    specific_id,  // Optimizer chooses best lots (default for direct indexing)
    fifo,
    lifo
};

enum class TaxCharacter { short_term, long_term };

/// Row-major dense matrix (Eigen-free public surface).
struct DenseMatrix {
    std::size_t rows{};
    std::size_t cols{};
    std::vector<double> values;  // row-major, size = rows * cols

    [[nodiscard]] double operator()(std::size_t r, std::size_t c) const {
        return values[r * cols + c];
    }

    double& operator()(std::size_t r, std::size_t c) {
        return values[r * cols + c];
    }
};

// ---------------------------------------------------------------------------
// Tax lots
// ---------------------------------------------------------------------------

struct TaxLot {
    LotId lot_id{};
    AssetId asset_id;
    double shares{};
    double cost_basis_per_share{};
    Date acquired_on{};
};

// ---------------------------------------------------------------------------
// Portfolio state
// ---------------------------------------------------------------------------

struct PortfolioState {
    Date as_of{};
    double cash{};
    std::vector<TaxLot> lots;
};

// ---------------------------------------------------------------------------
// Risk models
// ---------------------------------------------------------------------------

/// Pre-computed full covariance matrix.
struct FullCovarianceRisk {
    std::vector<AssetId> asset_ids;
    DenseMatrix covariance;  // N x N
};

/// Factor risk model: Cov = B * F * B' + diag(D).
struct FactorRisk {
    std::vector<AssetId> asset_ids;
    std::vector<std::string> factor_ids;
    DenseMatrix exposures;          // N x K
    DenseMatrix factor_covariance;  // K x K
    std::vector<double> specific_variance;  // N
};

using RiskModel = std::variant<FullCovarianceRisk, FactorRisk>;

// ---------------------------------------------------------------------------
// Market data
// ---------------------------------------------------------------------------

struct MarketData {
    Date as_of{};
    std::vector<AssetId> asset_ids;
    std::vector<double> prices;              // N
    std::vector<double> benchmark_weights;   // N
    std::vector<double> transaction_cost_bps; // N (basis points)
    RiskModel risk_model;
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct WeightBound {
    double min_weight{0.0};
    double max_weight{1.0};
};

struct Constraints {
    double max_turnover{1.0};
    double cash_buffer{0.0};
    std::unordered_map<AssetId, WeightBound> weight_bounds;
    std::unordered_set<AssetId> no_buy;
    std::unordered_set<AssetId> no_sell;
};

struct TaxConfig {
    DisposalMethod disposal_method{DisposalMethod::specific_id};
    double short_term_rate{0.37};      // US default; Japan: 0.20315
    double long_term_rate{0.20};       // US default; Japan: 0.20315 (no distinction)
    bool allow_loss_harvesting{true};
    std::optional<int> wash_sale_window_days{30};
};

struct ObjectiveWeights {
    double allocation_drift{1.0};
    double transaction_cost{1.0};
    double tax_cost{1.0};
    double alpha_weight{0.0};           // λ_alpha (0 = disabled)
    std::vector<double> alpha_vector;   // Per-asset expected returns (empty = no views)
};

struct OptimizationConfig {
    Constraints constraints;
    TaxConfig taxes;
    ObjectiveWeights objective;
    double min_trade_notional{0.0};
    bool round_to_whole_shares{false};
    std::string solver{"osqp"};  // "osqp" or "scs"
};

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

struct RebalanceRequest {
    PortfolioState portfolio;
    MarketData market;
    OptimizationConfig config;
};

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

struct Trade {
    AssetId asset_id;
    Side side{Side::buy};
    double shares{};
    double notional{};
};

struct LotDisposition {
    LotId lot_id{};
    AssetId asset_id;
    double shares_sold{};
    double proceeds{};
    double cost_basis{};
    double realized_gain{};
    TaxCharacter tax_character{TaxCharacter::long_term};
    double tax_liability{};
};

struct Diagnostics {
    bool converged{false};
    std::string solver_status;
    double objective_value{};
    double ex_ante_allocation_drift{};
    double turnover{};
    double estimated_transaction_cost{};
    double estimated_tax_cost{};
};

struct RebalanceResult {
    std::vector<Trade> trades;
    std::vector<LotDisposition> lot_dispositions;
    std::vector<double> target_weights;  // same order as market.asset_ids
    Diagnostics diagnostics;
};

// ---------------------------------------------------------------------------
// Core function
// ---------------------------------------------------------------------------

/// Plan a rebalance: given portfolio state, market data, and config,
/// compute the optimal trade list with tax lot dispositions.
///
/// This is the single entry point for the optimization engine.
[[nodiscard]] RebalanceResult plan_rebalance(const RebalanceRequest& request);

} // namespace ov
