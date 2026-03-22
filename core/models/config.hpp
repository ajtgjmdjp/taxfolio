#pragma once

#include "core/models/types.hpp"
#include "core/optimizer/optimizer.hpp"
#include <string>
#include <unordered_map>

namespace openvolt {

/// Top-level engine configuration.
struct EngineConfig {
    // General
    Money initial_investment = 100'000'000;  // 1 oku yen
    std::string rebalance_frequency = "weekly";  // "weekly" or "monthly"
    int num_holdings = 300;

    // Risk model
    std::string risk_method = "factor";  // "factor", "ewma", "shrinkage", "blend", "sample"
    int risk_window = 252;
    double risk_ewma_lambda = 0.97;
    int factor_n_factors = 5;
    int factor_cov_window = 756;
    int factor_cov_halflife = 120;
    double blend_ratio = 0.7;

    // Optimization
    OptimizationParams optimization;

    // TLH
    bool enable_tlh = true;
    double tlh_min_loss_pct = 0.05;
    double tlh_max_harvest_pct = 0.50;
    bool tlh_daily = true;
    std::string tlh_buyback_mode = "buyback";  // "buyback" or "substitute"

    // Emergency rebalance
    bool enable_emergency_rebalance = false;
    double te_critical_threshold = 0.025;
    int emergency_cooldown = 5;

    // Sector customization (sector_name -> multiplier, 0 = exclude)
    std::unordered_map<std::string, double> sector_customize;
};

} // namespace openvolt
