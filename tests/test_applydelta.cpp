#include <gtest/gtest.h>
#include "orderbook/orderbook.hpp"
#include "engine/apply_delta.hpp"

// -----------------------------------------------------------------------------
// Fixture for applyDelta tests.
// -----------------------------------------------------------------------------
class ApplyDeltaTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialise with a dummy book; reset will set proper window.
        book = std::make_unique<OrderBook>(0, 0, 1000);
        book->reset(500, 600, 2000);  // center around 550, window size 2000
    }

    std::unique_ptr<OrderBook> book;
};

// -----------------------------------------------------------------------------
// 1. Insert a single bid and ask.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, InsertBidAndAsk) {
    Delta d;
    d.bids.emplace_back(550, 10);
    d.asks.emplace_back(650, 20);

    applyDelta(d, *book);

    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 650);

    auto snap = book->snapshotActiveLevels();

    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].first, 550);
    EXPECT_EQ(snap.bids[0].second, 10);

    ASSERT_EQ(snap.asks.size(), 1);
    EXPECT_EQ(snap.asks[0].first, 650);
    EXPECT_EQ(snap.asks[0].second, 20);
}

// -----------------------------------------------------------------------------
// 2. Update an existing bid and ask.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, UpdateBidAndAsk) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    d1.asks.emplace_back(650, 20);
    applyDelta(d1, *book);

    Delta d2;
    d2.bids.emplace_back(550, 50);
    d2.asks.emplace_back(650, 60);
    applyDelta(d2, *book);

    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 650);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].second, 50);
    ASSERT_EQ(snap.asks.size(), 1);
    EXPECT_EQ(snap.asks[0].second, 60);
}

// -----------------------------------------------------------------------------
// 3. Delete a bid and ask (set quantity to 0).
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, DeleteBidAndAsk) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    d1.asks.emplace_back(650, 20);
    applyDelta(d1, *book);

    Delta d2;
    d2.bids.emplace_back(550, 0);
    d2.asks.emplace_back(650, 0);
    applyDelta(d2, *book);

    EXPECT_EQ(book->bestBid(), -1);
    EXPECT_EQ(book->bestAsk(), -1);

    auto snap = book->snapshotActiveLevels();
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

// -----------------------------------------------------------------------------
// 4. Insert multiple levels in one Delta.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, MultipleLevelsInOneDelta) {
    Delta d;
    d.bids.emplace_back(550, 10);
    d.bids.emplace_back(560, 20);
    d.bids.emplace_back(540, 30);
    d.asks.emplace_back(650, 40);
    d.asks.emplace_back(640, 50);
    d.asks.emplace_back(660, 60);

    applyDelta(d, *book);

    EXPECT_EQ(book->bestBid(), 560);
    EXPECT_EQ(book->bestAsk(), 640);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 3);
    EXPECT_EQ(snap.bids[0].first, 560);
    EXPECT_EQ(snap.bids[1].first, 550);
    EXPECT_EQ(snap.bids[2].first, 540);

    ASSERT_EQ(snap.asks.size(), 3);
    EXPECT_EQ(snap.asks[0].first, 640);
    EXPECT_EQ(snap.asks[1].first, 650);
    EXPECT_EQ(snap.asks[2].first, 660);
}

// -----------------------------------------------------------------------------
// 5. Apply multiple deltas sequentially.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, MultipleDeltasSequential) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    applyDelta(d1, *book);
    EXPECT_EQ(book->bestBid(), 550);

    Delta d2;
    d2.bids.emplace_back(560, 20);
    applyDelta(d2, *book);
    EXPECT_EQ(book->bestBid(), 560);

    Delta d3;
    d3.bids.emplace_back(560, 0);
    applyDelta(d3, *book);
    EXPECT_EQ(book->bestBid(), 550);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].first, 550);
}

// -----------------------------------------------------------------------------
// 6. Empty Delta (no bids/asks) should do nothing.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, EmptyDelta) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    applyDelta(d1, *book);
    EXPECT_EQ(book->bestBid(), 550);

    Delta empty;
    applyDelta(empty, *book);
    EXPECT_EQ(book->bestBid(), 550);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].first, 550);
}

// -----------------------------------------------------------------------------
// 7. Delete a level that doesn't exist (should be safe).
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, DeleteNonexistentLevel) {
    Delta d;
    d.bids.emplace_back(550, 0);
    d.asks.emplace_back(650, 0);
    applyDelta(d, *book);
    EXPECT_EQ(book->bestBid(), -1);
    EXPECT_EQ(book->bestAsk(), -1);

    auto snap = book->snapshotActiveLevels();
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

// -----------------------------------------------------------------------------
// 8. Update best bid/ask with multiple levels.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, UpdateBestBidAsk) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    d1.asks.emplace_back(650, 20);
    applyDelta(d1, *book);
    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 650);

    Delta d2;
    d2.bids.emplace_back(560, 30);
    d2.asks.emplace_back(640, 40);
    applyDelta(d2, *book);
    EXPECT_EQ(book->bestBid(), 560);
    EXPECT_EQ(book->bestAsk(), 640);

    Delta d3;
    d3.bids.emplace_back(560, 0);
    d3.asks.emplace_back(640, 0);
    applyDelta(d3, *book);
    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 650);
}

// -----------------------------------------------------------------------------
// 9. Large number of levels (stress test).
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, LargeDelta) {
    Delta d;
    constexpr size_t N = 1000;
    for (size_t i = 1; i <= N; ++i) {
        d.bids.emplace_back(500 + i, i);
        d.asks.emplace_back(600 + i, i);
    }
    applyDelta(d, *book);

    EXPECT_EQ(book->bestBid(), 1500);
    EXPECT_EQ(book->bestAsk(), 601);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), N);
    ASSERT_EQ(snap.asks.size(), N);

    EXPECT_EQ(snap.bids.front().first, 1500);
    EXPECT_EQ(snap.bids.front().second, 1000);
    EXPECT_EQ(snap.bids.back().first, 501);
    EXPECT_EQ(snap.bids.back().second, 1);

    EXPECT_EQ(snap.asks.front().first, 601);
    EXPECT_EQ(snap.asks.front().second, 1);
    EXPECT_EQ(snap.asks.back().first, 1600);
    EXPECT_EQ(snap.asks.back().second, 1000);

    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(snap.bids[i].first, 1500 - i);
        EXPECT_EQ(snap.bids[i].second, 1000 - i);
        EXPECT_EQ(snap.asks[i].first, 601 + i);
        EXPECT_EQ(snap.asks[i].second, 1 + i);
    }
}

// -----------------------------------------------------------------------------
// 10. Duplicate price within same delta – last update wins.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, DuplicatePriceWithinSameDelta) {
    Delta d;
    d.bids.emplace_back(550, 10);
    d.bids.emplace_back(550, 20);  // same price, later update should overwrite

    applyDelta(d, *book);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].first, 550);
    EXPECT_EQ(snap.bids[0].second, 20);
}

// -----------------------------------------------------------------------------
// 11. Delete best level promotes next best.
// -----------------------------------------------------------------------------
TEST_F(ApplyDeltaTest, DeleteBestLevelPromotesNextLevel) {
    Delta d1;
    d1.bids.emplace_back(550, 10);
    d1.bids.emplace_back(560, 20);
    d1.asks.emplace_back(650, 30);
    d1.asks.emplace_back(640, 40);
    applyDelta(d1, *book);

    EXPECT_EQ(book->bestBid(), 560);
    EXPECT_EQ(book->bestAsk(), 640);

    Delta d2;
    d2.bids.emplace_back(560, 0);
    d2.asks.emplace_back(640, 0);
    applyDelta(d2, *book);

    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 650);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 1);
    EXPECT_EQ(snap.bids[0].first, 550);
    EXPECT_EQ(snap.bids[0].second, 10);

    ASSERT_EQ(snap.asks.size(), 1);
    EXPECT_EQ(snap.asks[0].first, 650);
    EXPECT_EQ(snap.asks[0].second, 30);
}