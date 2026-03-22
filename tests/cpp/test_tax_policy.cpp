#include <gtest/gtest.h>
#include "core/models/tax_policy.hpp"
#include <unordered_map>

using namespace openvolt;

static Date make_date(int year, unsigned month, unsigned day) {
    return Date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
}

// ---------------------------------------------------------------------------
// Japan Tax Policy
// ---------------------------------------------------------------------------

TEST(JapanTaxPolicyTest, EstimateTaxCosts) {
    JapanTaxPolicy policy(0.20315);

    std::unordered_map<Ticker, TaxLotCollection> lots;
    lots["7203"].add_lot({make_date(2025, 1, 1), 100.0, 2500.0});
    lots["6758"].add_lot({make_date(2025, 1, 1), 50.0, 3500.0});

    std::unordered_map<Ticker, Price> prices = {{"7203", 2800.0}, {"6758", 3200.0}};
    double total = 100 * 2800 + 50 * 3200;  // 440000

    auto costs = policy.estimate_tax_costs(lots, prices, total, make_date(2026, 3, 21));
    ASSERT_EQ(costs.size(), 2);

    // 7203: gain = 100*(2800-2500) = 30000, tax = 30000*0.20315 = 6094.5
    // cost_per_unit = 6094.5 / 440000
    for (const auto& c : costs) {
        if (c.ticker == "7203") {
            EXPECT_GT(c.cost_per_unit, 0.0);  // Gain -> positive tax cost
        }
        if (c.ticker == "6758") {
            EXPECT_LT(c.cost_per_unit, 0.0);  // Loss -> negative (benefit)
        }
    }
}

TEST(JapanTaxPolicyTest, DisposeLots_AverageCost) {
    JapanTaxPolicy policy(0.20315);

    TaxLotCollection lots;
    lots.add_lot({make_date(2025, 1, 1), 50.0, 100.0});   // Lot at 100
    lots.add_lot({make_date(2025, 6, 1), 50.0, 200.0});   // Lot at 200

    // Average cost = (50*100 + 50*200) / 100 = 150
    auto result = policy.dispose_lots(lots, 30.0, 180.0, make_date(2026, 3, 17));

    ASSERT_EQ(result.entries.size(), 1);
    auto& e = result.entries[0];
    EXPECT_DOUBLE_EQ(e.shares_sold, 30.0);
    EXPECT_DOUBLE_EQ(e.proceeds, 30.0 * 180.0);
    // Cost basis = 30 * 150 (average) = 4500
    EXPECT_DOUBLE_EQ(e.cost_basis, 30.0 * 150.0);
    // Gain = 5400 - 4500 = 900
    EXPECT_NEAR(e.realized_gain, 900.0, 0.01);
    EXPECT_GT(e.tax_liability, 0.0);
    EXPECT_FALSE(e.is_long_term);  // Japan has no distinction
}

TEST(JapanTaxPolicyTest, NoWashSale) {
    JapanTaxPolicy policy;
    auto result = policy.check_wash_sale("7203", make_date(2026, 3, 17), {});
    EXPECT_FALSE(result.is_wash_sale);
}

// ---------------------------------------------------------------------------
// US Tax Policy
// ---------------------------------------------------------------------------

TEST(USTaxPolicyTest, LongTermVsShortTerm) {
    USTaxPolicy policy(0.37, 0.20, 30);

    TaxLotCollection lots;
    lots.add_lot({make_date(2024, 1, 1), 50.0, 100.0});   // Long-term (>1yr ago)
    lots.add_lot({make_date(2026, 2, 1), 50.0, 80.0});    // Short-term (<1yr)

    auto result = policy.dispose_lots(lots, 30.0, 120.0, make_date(2026, 3, 17));

    // Should sell short-term loss lot first (80 -> 120 is gain, but
    // the tax-optimal sort prioritizes losses)
    // Both lots have gains at price 120, so highest cost first
    ASSERT_GT(result.entries.size(), 0);
    EXPECT_GT(result.total_tax, 0.0);

    // Check that long-term vs short-term is correctly identified
    for (const auto& e : result.entries) {
        if (e.cost_basis / e.shares_sold < 90.0) {
            // Low cost lot = acquired 2026-02-01 = short-term
            EXPECT_FALSE(e.is_long_term);
        }
    }
}

TEST(USTaxPolicyTest, WashSaleDetection) {
    USTaxPolicy policy(0.37, 0.20, 30);

    // Simulate a recent sale with loss
    LotDispositionResult::Entry recent_sale{
        1, "AAPL", 10.0, 1500.0, 2000.0, -500.0, false, 0.0
    };

    auto check = policy.check_wash_sale("AAPL", make_date(2026, 3, 17), {recent_sale});
    EXPECT_TRUE(check.is_wash_sale);
    EXPECT_EQ(check.disallowed_loss, 500.0);
}

TEST(USTaxPolicyTest, NoWashSaleForDifferentTicker) {
    USTaxPolicy policy(0.37, 0.20, 30);

    LotDispositionResult::Entry recent_sale{
        1, "AAPL", 10.0, 1500.0, 2000.0, -500.0, false, 0.0
    };

    auto check = policy.check_wash_sale("MSFT", make_date(2026, 3, 17), {recent_sale});
    EXPECT_FALSE(check.is_wash_sale);
}

TEST(USTaxPolicyTest, NoWashSaleForGain) {
    USTaxPolicy policy(0.37, 0.20, 30);

    LotDispositionResult::Entry recent_sale{
        1, "AAPL", 10.0, 2000.0, 1500.0, 500.0, true, 100.0
    };

    auto check = policy.check_wash_sale("AAPL", make_date(2026, 3, 17), {recent_sale});
    EXPECT_FALSE(check.is_wash_sale);
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

TEST(TaxPolicyFactoryTest, CreatePolicies) {
    auto jp = make_tax_policy("japan");
    EXPECT_EQ(jp->name(), "japan");

    auto jp2 = make_tax_policy("jp");
    EXPECT_EQ(jp2->name(), "japan");

    auto us = make_tax_policy("us");
    EXPECT_EQ(us->name(), "us");

    EXPECT_THROW(make_tax_policy("unknown"), std::invalid_argument);
}
