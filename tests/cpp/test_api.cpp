#include <gtest/gtest.h>
#include "api.hpp"
#include <cmath>

using namespace ov;

static Date make_date(int year, int month, int day) {
    return std::chrono::sys_days{
        std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)}
        / std::chrono::day{static_cast<unsigned>(day)}
    };
}

class PlanRebalanceTest : public ::testing::Test {
protected:
    RebalanceRequest make_simple_request() {
        // 3-asset universe: AAPL, MSFT, NVDA
        const int N = 3;

        // Portfolio: holds AAPL and MSFT, no NVDA
        PortfolioState portfolio;
        portfolio.as_of = make_date(2026, 3, 16);
        portfolio.cash = 5000.0;
        portfolio.lots = {
            {1, "AAPL", 50.0, 160.0, make_date(2025, 1, 15)},  // Long-term lot
            {2, "MSFT", 30.0, 400.0, make_date(2025, 6, 1)},   // Long-term lot
            {3, "AAPL", 20.0, 220.0, make_date(2026, 2, 1)},   // Short-term lot (loss at 200)
        };

        // Market data
        MarketData market;
        market.as_of = make_date(2026, 3, 16);
        market.asset_ids = {"AAPL", "MSFT", "NVDA"};
        market.prices = {200.0, 450.0, 900.0};
        market.benchmark_weights = {0.40, 0.35, 0.25};
        market.transaction_cost_bps = {2.0, 2.0, 5.0};

        // Simple covariance (identity-like with some correlation)
        FullCovarianceRisk risk;
        risk.asset_ids = {"AAPL", "MSFT", "NVDA"};
        risk.covariance.rows = 3;
        risk.covariance.cols = 3;
        risk.covariance.values = {
            0.04,  0.01,  0.005,
            0.01,  0.03,  0.008,
            0.005, 0.008, 0.06,
        };
        market.risk_model = std::move(risk);

        // Config
        OptimizationConfig config;
        config.constraints.max_turnover = 0.50;
        config.constraints.cash_buffer = 2000.0;
        config.taxes.disposal_method = DisposalMethod::specific_id;
        config.taxes.short_term_rate = 0.20315;  // Japan
        config.taxes.long_term_rate = 0.20315;
        config.objective.allocation_drift = 1.0;
        config.objective.transaction_cost = 0.5;
        config.objective.tax_cost = 0.8;
        config.min_trade_notional = 100.0;

        return {std::move(portfolio), std::move(market), std::move(config)};
    }
};

TEST_F(PlanRebalanceTest, SolvesSuccessfully) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    EXPECT_TRUE(result.diagnostics.converged);
    EXPECT_EQ(result.diagnostics.solver_status, "solved");
    EXPECT_EQ(result.target_weights.size(), 3);
}

TEST_F(PlanRebalanceTest, WeightsSumToInvestFraction) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    ASSERT_TRUE(result.diagnostics.converged);

    double weight_sum = 0.0;
    for (double w : result.target_weights) {
        weight_sum += w;
        EXPECT_GE(w, -1e-6);  // No negative weights
    }

    // Portfolio value: 50*200 + 30*450 + 20*200 + 5000 = 10000+13500+4000+5000 = 32500
    // Cash buffer = 2000 → invest fraction = 1 - 2000/32500 ≈ 0.9385
    double total_value = 50.0 * 200.0 + 30.0 * 450.0 + 20.0 * 200.0 + 5000.0;
    double invest_fraction = 1.0 - 2000.0 / total_value;

    EXPECT_NEAR(weight_sum, invest_fraction, 1e-4);
}

TEST_F(PlanRebalanceTest, TradesAreGenerated) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    ASSERT_TRUE(result.diagnostics.converged);
    // Should generate some trades since portfolio doesn't match benchmark
    EXPECT_GT(result.trades.size(), 0);
}

TEST_F(PlanRebalanceTest, LotDispositionsForSells) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    ASSERT_TRUE(result.diagnostics.converged);

    // Check all sell trades have corresponding lot dispositions
    for (const auto& trade : result.trades) {
        if (trade.side == Side::sell) {
            bool has_disposition = false;
            for (const auto& disp : result.lot_dispositions) {
                if (disp.asset_id == trade.asset_id) {
                    has_disposition = true;
                    EXPECT_GT(disp.shares_sold, 0.0);
                    EXPECT_GT(disp.proceeds, 0.0);
                    break;
                }
            }
            EXPECT_TRUE(has_disposition)
                << "Sell trade for " << trade.asset_id << " has no lot disposition";
        }
    }
}

TEST_F(PlanRebalanceTest, TrackingErrorIsReasonable) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    ASSERT_TRUE(result.diagnostics.converged);
    // TE should be positive but reasonable (< 100% annualized)
    // Higher TE is expected when turnover penalty constrains rebalancing
    EXPECT_GT(result.diagnostics.ex_ante_allocation_drift, 0.0);
    EXPECT_LT(result.diagnostics.ex_ante_allocation_drift, 1.00);
}

TEST_F(PlanRebalanceTest, TurnoverRespected) {
    auto req = make_simple_request();
    auto result = plan_rebalance(req);

    ASSERT_TRUE(result.diagnostics.converged);
    EXPECT_LE(result.diagnostics.turnover, 0.50 + 1e-4);
}

TEST_F(PlanRebalanceTest, EmptyPortfolio) {
    RebalanceRequest req;
    req.portfolio.as_of = make_date(2026, 3, 16);
    req.portfolio.cash = 0.0;
    req.market.as_of = make_date(2026, 3, 16);
    req.market.asset_ids = {"AAPL"};
    req.market.prices = {200.0};
    req.market.benchmark_weights = {1.0};
    req.market.transaction_cost_bps = {2.0};
    FullCovarianceRisk risk;
    risk.asset_ids = {"AAPL"};
    risk.covariance = {1, 1, {0.04}};
    req.market.risk_model = std::move(risk);

    auto result = plan_rebalance(req);
    EXPECT_FALSE(result.diagnostics.converged);
    EXPECT_EQ(result.diagnostics.solver_status, "empty_portfolio");
}

TEST_F(PlanRebalanceTest, FactorRiskModel) {
    auto req = make_simple_request();

    // Replace with factor risk model
    FactorRisk frisk;
    frisk.asset_ids = {"AAPL", "MSFT", "NVDA"};
    frisk.factor_ids = {"mkt", "size"};
    frisk.exposures = {3, 2, {1.0, -0.1, 0.95, 0.05, 1.3, 0.2}};
    frisk.factor_covariance = {2, 2, {0.04, 0.002, 0.002, 0.015}};
    frisk.specific_variance = {0.03, 0.025, 0.06};
    req.market.risk_model = std::move(frisk);

    auto result = plan_rebalance(req);
    EXPECT_TRUE(result.diagnostics.converged);
}
