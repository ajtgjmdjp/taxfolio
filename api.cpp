#include "api.hpp"
#include "core/optimizer/optimizer.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace ov {

// ---------------------------------------------------------------------------
// Internal: convert public types to Eigen
// ---------------------------------------------------------------------------

static Eigen::MatrixXd to_eigen(const DenseMatrix& m) {
    Eigen::MatrixXd result(m.rows, m.cols);
    for (std::size_t r = 0; r < m.rows; ++r) {
        for (std::size_t c = 0; c < m.cols; ++c) {
            result(r, c) = m(r, c);
        }
    }
    return result;
}

static Eigen::VectorXd to_eigen_vec(const std::vector<double>& v) {
    return Eigen::Map<const Eigen::VectorXd>(v.data(), static_cast<Eigen::Index>(v.size()));
}

// ---------------------------------------------------------------------------
// Internal: build covariance matrix from risk model variant
// ---------------------------------------------------------------------------

static Eigen::MatrixXd build_covariance(const RiskModel& risk_model) {
    return std::visit([](const auto& rm) -> Eigen::MatrixXd {
        using T = std::decay_t<decltype(rm)>;
        if constexpr (std::is_same_v<T, FullCovarianceRisk>) {
            return to_eigen(rm.covariance);
        } else {
            // FactorRisk: Cov = B * F * B' + diag(D)
            const auto B = to_eigen(rm.exposures);
            const auto F = to_eigen(rm.factor_covariance);
            const auto D = to_eigen_vec(rm.specific_variance);
            return (B * F * B.transpose()).eval()
                   + Eigen::MatrixXd(D.asDiagonal());
        }
    }, risk_model);
}

// ---------------------------------------------------------------------------
// Internal: derive positions from lots
// ---------------------------------------------------------------------------

struct DerivedPosition {
    double shares{};
    double cost_basis{};
    double market_value{};
    double unrealized_gain{};
};

static std::unordered_map<AssetId, DerivedPosition> derive_positions(
    const std::vector<TaxLot>& lots,
    const std::vector<AssetId>& asset_ids,
    const std::vector<double>& prices
) {
    // Price lookup
    std::unordered_map<AssetId, double> price_map;
    for (std::size_t i = 0; i < asset_ids.size(); ++i) {
        price_map[asset_ids[i]] = prices[i];
    }

    std::unordered_map<AssetId, DerivedPosition> positions;
    for (const auto& lot : lots) {
        auto& pos = positions[lot.asset_id];
        pos.shares += lot.shares;
        pos.cost_basis += lot.shares * lot.cost_basis_per_share;
        auto pit = price_map.find(lot.asset_id);
        if (pit != price_map.end()) {
            pos.market_value += lot.shares * pit->second;
            pos.unrealized_gain += lot.shares * (pit->second - lot.cost_basis_per_share);
        }
    }
    return positions;
}

// ---------------------------------------------------------------------------
// plan_rebalance implementation
// ---------------------------------------------------------------------------

RebalanceResult plan_rebalance(const RebalanceRequest& request) {
    const auto& portfolio = request.portfolio;
    const auto& market = request.market;
    const auto& config = request.config;
    const int N = static_cast<int>(market.asset_ids.size());

    RebalanceResult result;
    result.target_weights.resize(N, 0.0);

    // Derive positions from lots
    auto positions = derive_positions(portfolio.lots, market.asset_ids, market.prices);

    // Compute total portfolio value
    double total_value = portfolio.cash;
    for (const auto& [id, pos] : positions) {
        total_value += pos.market_value;
    }

    if (total_value <= 0.0) {
        result.diagnostics.solver_status = "empty_portfolio";
        return result;
    }

    // Current weights
    Eigen::VectorXd w_current = Eigen::VectorXd::Zero(N);
    for (int i = 0; i < N; ++i) {
        auto it = positions.find(market.asset_ids[i]);
        if (it != positions.end()) {
            w_current(i) = it->second.market_value / total_value;
        }
    }

    // Boyd-lite: compute tax-adjusted unrealized gains using lot-level ST/LT rates.
    // Instead of naive (gain * flat_rate), each lot's gain is weighted by its
    // actual tax rate (ST vs LT based on holding period).
    Eigen::VectorXd unrealized_gains = Eigen::VectorXd::Zero(N);
    {
        auto as_of_days = portfolio.as_of;
        double st_rate = config.taxes.short_term_rate;
        double lt_rate = config.taxes.long_term_rate;

        // Per-asset: sum of (lot_gain * lot_rate) across all lots
        std::unordered_map<AssetId, double> tax_weighted_gains;
        for (const auto& lot : portfolio.lots) {
            auto pit = std::find(market.asset_ids.begin(), market.asset_ids.end(), lot.asset_id);
            if (pit == market.asset_ids.end()) continue;
            int idx = static_cast<int>(pit - market.asset_ids.begin());
            double price = market.prices[idx];
            double gain = lot.shares * (price - lot.cost_basis_per_share);

            // Determine ST vs LT from holding period
            bool is_lt = (as_of_days - lot.acquired_on).count() > 365;
            double rate = is_lt ? lt_rate : st_rate;

            // Tax-weighted gain: positive gains are penalized, losses are beneficial
            tax_weighted_gains[lot.asset_id] += gain * rate;
        }

        // Map to optimizer vector (normalized by total_value and tax_rate)
        for (int i = 0; i < N; ++i) {
            auto it = tax_weighted_gains.find(market.asset_ids[i]);
            if (it != tax_weighted_gains.end() && st_rate > 1e-12) {
                // The optimizer multiplies by tax_rate internally, so divide it out.
                // Clamp to >= 0: negative values (loss harvesting benefit) cannot be
                // correctly represented with symmetric |Δw| — would reward buys too.
                // Loss harvesting requires buy/sell variable split (Boyd-lite v1).
                unrealized_gains(i) = std::max(0.0, it->second / (st_rate * total_value));
            }
        }
    }

    // Benchmark weights
    const Eigen::VectorXd w_bench = to_eigen_vec(market.benchmark_weights);

    // Covariance matrix
    const Eigen::MatrixXd cov = build_covariance(market.risk_model);

    // Transaction costs (convert bps to fraction)
    Eigen::VectorXd tcost_frac = Eigen::VectorXd::Zero(N);
    if (!market.transaction_cost_bps.empty()) {
        for (int i = 0; i < N; ++i) {
            tcost_frac(i) = market.transaction_cost_bps[i] / 10000.0;
        }
    }

    // -----------------------------------------------------------------------
    // Trace normalization: scale covariance so trace(Cov) = N
    // This makes lambda_drift independent of universe size and volatility level
    // -----------------------------------------------------------------------
    double cov_scale = 1.0;
    const double cov_trace = cov.trace();
    if (cov_trace > 1e-12) {
        cov_scale = static_cast<double>(N) / cov_trace;
    }
    const Eigen::MatrixXd cov_scaled = cov * cov_scale;

    // -----------------------------------------------------------------------
    // Build OptimizationParams from config
    // -----------------------------------------------------------------------
    openvolt::OptimizationParams opt_params;
    opt_params.lambda_drift = config.objective.allocation_drift;
    opt_params.lambda_tcost = config.objective.transaction_cost;
    opt_params.lambda_tax = config.objective.tax_cost;
    opt_params.tax_rate = config.taxes.short_term_rate;
    opt_params.turnover_cap = config.constraints.max_turnover;
    opt_params.weight_cap = 1.0;  // Default, overridden by per-asset bounds
    opt_params.invest_fraction = std::max(0.0, 1.0 - config.constraints.cash_buffer / total_value);

    // Alpha (expected return views)
    if (!config.objective.alpha_vector.empty() && config.objective.alpha_weight > 0.0) {
        opt_params.alpha = to_eigen_vec(config.objective.alpha_vector);
        opt_params.lambda_alpha = config.objective.alpha_weight;
    }

    // Per-asset transaction costs
    opt_params.tcost_frac = tcost_frac;

    // Per-asset weight bounds
    for (int i = 0; i < N; ++i) {
        auto it = config.constraints.weight_bounds.find(market.asset_ids[i]);
        if (it != config.constraints.weight_bounds.end()) {
            opt_params.per_asset_bounds[i] = {it->second.min_weight, it->second.max_weight};
        } else {
            opt_params.per_asset_bounds[i] = {0.0, 1.0};
        }
    }

    // No-buy / no-sell
    for (int i = 0; i < N; ++i) {
        if (config.constraints.no_buy.count(market.asset_ids[i])) {
            opt_params.no_buy.insert(i);
        }
        if (config.constraints.no_sell.count(market.asset_ids[i])) {
            opt_params.no_sell.insert(i);
        }
    }

    // -----------------------------------------------------------------------
    // Solve via Optimizer interface (OSQP or SCS)
    // -----------------------------------------------------------------------
    std::unique_ptr<openvolt::Optimizer> optimizer;
    try {
        optimizer = openvolt::make_optimizer(config.solver);
    } catch (const std::invalid_argument&) {
        // Unknown solver — fall back to OSQP
        optimizer = openvolt::make_optimizer("osqp");
    }
    auto opt_result = optimizer->solve(w_bench, w_current, cov_scaled, unrealized_gains, opt_params);

    result.diagnostics.converged = opt_result.converged;
    result.diagnostics.solver_status = opt_result.solver_status;
    result.diagnostics.objective_value = opt_result.objective_value;
    result.diagnostics.turnover = opt_result.predicted_turnover;

    // Recompute TE using original (non-normalized) covariance
    if (opt_result.converged) {
        Eigen::VectorXd active = opt_result.target_weights - w_bench;
        double variance = static_cast<double>(active.transpose() * cov * active);
        result.diagnostics.ex_ante_allocation_drift = std::sqrt(std::max(0.0, variance) * 252.0);
    }

    if (opt_result.converged) {
        // Extract target weights
        for (int i = 0; i < N; ++i) {
            result.target_weights[i] = std::max(0.0, opt_result.target_weights(i));
        }

        // Generate trades
        for (int i = 0; i < N; ++i) {
            double delta_weight = result.target_weights[i] - w_current(i);
            double delta_notional = delta_weight * total_value;

            if (std::abs(delta_notional) < config.min_trade_notional) {
                continue;
            }

            Trade trade;
            trade.asset_id = market.asset_ids[i];
            trade.side = delta_weight > 0.0 ? Side::buy : Side::sell;
            trade.notional = std::abs(delta_notional);
            trade.shares = trade.notional / market.prices[i];

            if (config.round_to_whole_shares) {
                trade.shares = std::round(trade.shares);
                trade.notional = trade.shares * market.prices[i];
            }

            if (trade.shares > 1e-10) {
                result.trades.push_back(trade);
            }
        }

        // Generate lot dispositions for sells
        // Group lots by asset, then select lots to sell
        std::unordered_map<AssetId, std::vector<const TaxLot*>> lots_by_asset;
        for (const auto& lot : portfolio.lots) {
            lots_by_asset[lot.asset_id].push_back(&lot);
        }

        for (const auto& trade : result.trades) {
            if (trade.side != Side::sell) continue;

            auto it = lots_by_asset.find(trade.asset_id);
            if (it == lots_by_asset.end()) continue;

            auto& lots = it->second;

            // Sort lots by disposal method
            switch (config.taxes.disposal_method) {
                case DisposalMethod::fifo:
                    std::sort(lots.begin(), lots.end(),
                        [](const TaxLot* a, const TaxLot* b) {
                            return a->acquired_on < b->acquired_on;
                        });
                    break;
                case DisposalMethod::lifo:
                    std::sort(lots.begin(), lots.end(),
                        [](const TaxLot* a, const TaxLot* b) {
                            return a->acquired_on > b->acquired_on;
                        });
                    break;
                case DisposalMethod::specific_id:
                default: {
                    double price = 0.0;
                    for (std::size_t i = 0; i < market.asset_ids.size(); ++i) {
                        if (market.asset_ids[i] == trade.asset_id) {
                            price = market.prices[i];
                            break;
                        }
                    }
                    // Tax optimal: losses first, then highest cost
                    std::sort(lots.begin(), lots.end(),
                        [price](const TaxLot* a, const TaxLot* b) {
                            double ga = price - a->cost_basis_per_share;
                            double gb = price - b->cost_basis_per_share;
                            if (ga < 0.0 && gb >= 0.0) return true;
                            if (ga >= 0.0 && gb < 0.0) return false;
                            if (ga < 0.0 && gb < 0.0) return ga < gb;
                            return a->cost_basis_per_share > b->cost_basis_per_share;
                        });
                    break;
                }
            }

            // Allocate sell across lots
            double remaining = trade.shares;
            double price = 0.0;
            for (std::size_t i = 0; i < market.asset_ids.size(); ++i) {
                if (market.asset_ids[i] == trade.asset_id) {
                    price = market.prices[i];
                    break;
                }
            }

            for (const auto* lot : lots) {
                if (remaining <= 1e-10) break;

                double sell_shares = std::min(remaining, lot->shares);
                double proceeds = sell_shares * price;
                double cost = sell_shares * lot->cost_basis_per_share;
                double gain = proceeds - cost;

                // Determine tax character based on holding period
                auto hold_days = (market.as_of - lot->acquired_on).count();
                TaxCharacter character = hold_days > 365
                    ? TaxCharacter::long_term
                    : TaxCharacter::short_term;

                double tax_rate = character == TaxCharacter::long_term
                    ? config.taxes.long_term_rate
                    : config.taxes.short_term_rate;

                double tax = gain > 0.0 ? gain * tax_rate : 0.0;

                result.lot_dispositions.push_back({
                    .lot_id = lot->lot_id,
                    .asset_id = lot->asset_id,
                    .shares_sold = sell_shares,
                    .proceeds = proceeds,
                    .cost_basis = cost,
                    .realized_gain = gain,
                    .tax_character = character,
                    .tax_liability = tax,
                });

                remaining -= sell_shares;
            }
        }

        // Compute aggregate tax/transaction cost diagnostics
        result.diagnostics.estimated_tax_cost = 0.0;
        for (const auto& disp : result.lot_dispositions) {
            result.diagnostics.estimated_tax_cost += disp.tax_liability;
        }

        result.diagnostics.estimated_transaction_cost = 0.0;
        for (const auto& trade : result.trades) {
            for (std::size_t i = 0; i < market.asset_ids.size(); ++i) {
                if (market.asset_ids[i] == trade.asset_id) {
                    result.diagnostics.estimated_transaction_cost +=
                        trade.notional * market.transaction_cost_bps[i] / 10000.0;
                    break;
                }
            }
        }
    }

    return result;
}

} // namespace ov
