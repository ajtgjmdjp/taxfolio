#include <gtest/gtest.h>
#include "core/models/tax_lot.hpp"

using namespace openvolt;

// Helper to make a Date
static Date make_date(int year, unsigned month, unsigned day) {
    return Date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
}

TEST(TaxLotTest, BasicLot) {
    TaxLot lot{make_date(2025, 1, 15), 100.0, 150.0};

    EXPECT_DOUBLE_EQ(lot.cost_basis(), 15000.0);
    EXPECT_DOUBLE_EQ(lot.unrealized_pnl(200.0), 5000.0);
    EXPECT_NEAR(lot.unrealized_pnl_pct(200.0), 1.0 / 3.0, 1e-10);
    EXPECT_DOUBLE_EQ(lot.unrealized_pnl(100.0), -5000.0);
}

TEST(TaxLotCollectionTest, AddAndTotal) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 100.0});
    coll.add_lot({make_date(2025, 6, 1), 50.0, 120.0});

    EXPECT_DOUBLE_EQ(coll.total_shares(), 150.0);
    EXPECT_DOUBLE_EQ(coll.total_cost_basis(), 100.0 * 100.0 + 50.0 * 120.0);
    EXPECT_EQ(coll.lot_count(), 2);
}

TEST(TaxLotCollectionTest, SellFIFO) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 100.0});   // Lot 1: bought at 100
    coll.add_lot({make_date(2025, 6, 1), 100.0, 150.0});   // Lot 2: bought at 150

    // Sell 80 shares FIFO at 200 — should sell from Lot 1 first
    Money cost = coll.sell(80.0, 200.0, LotDisposition::FIFO);
    EXPECT_DOUBLE_EQ(cost, 80.0 * 100.0);  // Cost basis = 80 * 100

    // Remaining: 20 shares at 100, 100 shares at 150
    EXPECT_DOUBLE_EQ(coll.total_shares(), 120.0);
}

TEST(TaxLotCollectionTest, SellLIFO) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 100.0});
    coll.add_lot({make_date(2025, 6, 1), 100.0, 150.0});

    // Sell 80 shares LIFO — should sell from Lot 2 first
    Money cost = coll.sell(80.0, 200.0, LotDisposition::LIFO);
    EXPECT_DOUBLE_EQ(cost, 80.0 * 150.0);

    EXPECT_DOUBLE_EQ(coll.total_shares(), 120.0);
}

TEST(TaxLotCollectionTest, SellHighestCost) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 50.0, 100.0});
    coll.add_lot({make_date(2025, 3, 1), 50.0, 200.0});
    coll.add_lot({make_date(2025, 6, 1), 50.0, 150.0});

    // Sell 60 shares HighestCost at 180
    // Should sell: 50 shares at 200, then 10 at 150
    Money cost = coll.sell(60.0, 180.0, LotDisposition::HighestCost);
    EXPECT_DOUBLE_EQ(cost, 50.0 * 200.0 + 10.0 * 150.0);

    EXPECT_DOUBLE_EQ(coll.total_shares(), 90.0);
}

TEST(TaxLotCollectionTest, SellTaxOptimal) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 50.0, 200.0});   // Loss at price 150
    coll.add_lot({make_date(2025, 3, 1), 50.0, 100.0});   // Gain at price 150
    coll.add_lot({make_date(2025, 6, 1), 50.0, 180.0});   // Loss at price 150

    // At market price 150: Lot1 has loss, Lot3 has loss, Lot2 has gain
    // Tax optimal should sell loss lots first
    Money cost = coll.sell(60.0, 150.0, LotDisposition::TaxOptimal);

    // Should sell: 50 shares at 200 (most loss), then 10 at 180
    EXPECT_DOUBLE_EQ(cost, 50.0 * 200.0 + 10.0 * 180.0);
}

TEST(TaxLotCollectionTest, HasLoss) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 200.0});

    EXPECT_TRUE(coll.has_loss(150.0));    // Market < cost
    EXPECT_FALSE(coll.has_loss(250.0));   // Market > cost
}

TEST(TaxLotCollectionTest, HarvestableLoss) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 200.0});  // Loss of 50/share at 150
    coll.add_lot({make_date(2025, 6, 1), 50.0, 100.0});   // Gain of 50/share at 150

    Money loss = coll.harvestable_loss(150.0);
    EXPECT_DOUBLE_EQ(loss, -5000.0);  // Only the loss lot: 100 * (150-200) = -5000
}

TEST(TaxLotCollectionTest, SellEntirePosition) {
    TaxLotCollection coll;
    coll.add_lot({make_date(2025, 1, 1), 100.0, 100.0});

    Money cost = coll.sell(100.0, 200.0, LotDisposition::FIFO);
    EXPECT_DOUBLE_EQ(cost, 10000.0);
    EXPECT_DOUBLE_EQ(coll.total_shares(), 0.0);
    EXPECT_EQ(coll.lot_count(), 0);
}
