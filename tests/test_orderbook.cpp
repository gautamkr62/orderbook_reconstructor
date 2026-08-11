#include <gtest/gtest.h>
#include "orderbook/orderbook.hpp"

// -----------------------------------------------------------------------------
// Fixture for OrderBook tests.
// -----------------------------------------------------------------------------
class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialise with a dummy state; we will call reset() in most tests.
        book = std::make_unique<OrderBook>(0, 0, 1000);
    }

    std::unique_ptr<OrderBook> book;
};

// -----------------------------------------------------------------------------
// 1. Constructor and reset()
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, ConstructorAndReset) {
    // ------------------------------------------------------------
    // 1. Populate the initial book heavily.
    // ------------------------------------------------------------
    book->reset(500, 600, 2000);

    book->updateBid(400, 10);
    book->updateBid(500, 20);
    book->updateBid(550, 30);
    book->updateBid(599, 40);

    book->updateAsk(601, 50);
    book->updateAsk(650, 60);
    book->updateAsk(700, 70);

    EXPECT_EQ(book->bestBid(), 599);
    EXPECT_EQ(book->bestAsk(), 601);

    auto before_reset = book->snapshotActiveLevels();

    ASSERT_EQ(before_reset.bids.size(), 4);
    ASSERT_EQ(before_reset.asks.size(), 3);

    // ------------------------------------------------------------
    // 2. Reset to a completely different price region.
    // ------------------------------------------------------------
    book->reset(10000, 10100, 2000);

    // Entire previous book must be gone.
    EXPECT_EQ(book->bestBid(), -1);
    EXPECT_EQ(book->bestAsk(), -1);

    auto after_reset = book->snapshotActiveLevels();

    EXPECT_TRUE(after_reset.bids.empty());
    EXPECT_TRUE(after_reset.asks.empty());

    // ------------------------------------------------------------
    // 3. Insert some levels in the new region.
    //    These are NEW insertions, not stale levels.
    // ------------------------------------------------------------
    book->updateBid(550, 999);
    book->updateAsk(601, 999);

    auto after_old_price_insert = book->snapshotActiveLevels();

    // ------------------------------------------------------------
    // 4. Check that OLD stale levels (not re-inserted) are gone.
    //    We should NOT check 550 or 601 because we just inserted them!
    // ------------------------------------------------------------
    for (const auto& [tick, qty] : after_old_price_insert.bids) {
        EXPECT_NE(tick, 400);  // old, not re-inserted
        EXPECT_NE(tick, 500);  // old, not re-inserted
        EXPECT_NE(tick, 599);  // old, not re-inserted
        // 550 IS present because we inserted it above.
    }

    for (const auto& [tick, qty] : after_old_price_insert.asks) {
        EXPECT_NE(tick, 650);  // old, not re-inserted
        EXPECT_NE(tick, 700);  // old, not re-inserted
        // 601 IS present because we inserted it above.
    }

    // ------------------------------------------------------------
    // 5. Insert a completely new book in the new region.
    // ------------------------------------------------------------
    book->updateBid(10000, 100);
    book->updateBid(10050, 200);
    book->updateBid(10099, 300);

    book->updateAsk(10100, 400);
    book->updateAsk(10150, 500);
    book->updateAsk(10199, 600);

    EXPECT_EQ(book->bestBid(), 10099);
    EXPECT_EQ(book->bestAsk(), 601);

    auto new_book = book->snapshotActiveLevels();

    ASSERT_EQ(new_book.bids.size(), 4); // 550, 10000, 10050, 10099
    ASSERT_EQ(new_book.asks.size(), 4); // 601, 10100, 10150, 10199

    // Bids descending.
    EXPECT_EQ(new_book.bids[0].first, 10099);
    EXPECT_EQ(new_book.bids[1].first, 10050);
    EXPECT_EQ(new_book.bids[2].first, 10000);
    EXPECT_EQ(new_book.bids[3].first, 550);   // 550 is still present

    // Asks ascending.
    EXPECT_EQ(new_book.asks[0].first, 601);   // 601 is still present
    EXPECT_EQ(new_book.asks[1].first, 10100);
    EXPECT_EQ(new_book.asks[2].first, 10150);
    EXPECT_EQ(new_book.asks[3].first, 10199);

    // ------------------------------------------------------------
    // 6. Reset AGAIN. This catches stale levels that survived
    // a previous reset/repopulation cycle.
    // ------------------------------------------------------------
    book->reset(20000, 20100, 2000);

    EXPECT_EQ(book->bestBid(), -1);
    EXPECT_EQ(book->bestAsk(), -1);

    auto final_book = book->snapshotActiveLevels();

    EXPECT_TRUE(final_book.bids.empty());
    EXPECT_TRUE(final_book.asks.empty());
}

// -----------------------------------------------------------------------------
// 2. updateBid / updateAsk
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, UpdateBidAndAsk) {
    book->reset(500, 600, 2000);

    // Insert new best bid
    book->updateBid(550, 10);
    EXPECT_EQ(book->bestBid(), 550);

    // Insert higher bid
    book->updateBid(560, 20);
    EXPECT_EQ(book->bestBid(), 560);

    // Insert lower bid
    book->updateBid(540, 30);
    EXPECT_EQ(book->bestBid(), 560); // unchanged

    // Delete best bid
    book->updateBid(560, 0);
    EXPECT_EQ(book->bestBid(), 550);

    // Update best ask
    book->updateAsk(550, 10);
    EXPECT_EQ(book->bestAsk(), 550);

    // Insert lower ask
    book->updateAsk(540, 20);
    EXPECT_EQ(book->bestAsk(), 540);

    // Delete best ask
    book->updateAsk(540, 0);
    EXPECT_EQ(book->bestAsk(), 550);
}

// -----------------------------------------------------------------------------
// 3. applySnapshot()
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, ApplySnapshot) {
    book->reset(500, 600, 2000);

    std::vector<std::pair<int64_t, uint64_t>> bids = {
        {500, 100},
        {499, 200},
        {498, 300}
    };
    std::vector<std::pair<int64_t, uint64_t>> asks = {
        {600, 100},
        {601, 200},
        {602, 300}
    };

    book->applySnapshot(bids, asks);

    EXPECT_EQ(book->bestBid(), 500);
    EXPECT_EQ(book->bestAsk(), 600);

    // Verify snapshot capture.
    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 3);
    EXPECT_EQ(snap.bids[0].first, 500);
    EXPECT_EQ(snap.bids[1].first, 499);
    EXPECT_EQ(snap.bids[2].first, 498);

    ASSERT_EQ(snap.asks.size(), 3);
    EXPECT_EQ(snap.asks[0].first, 600);
    EXPECT_EQ(snap.asks[1].first, 601);
    EXPECT_EQ(snap.asks[2].first, 602);
}

// -----------------------------------------------------------------------------
// 4. applySnapshot after reset clears old state
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, ApplySnapshotAfterResetClearsOldState) {
    book->reset(500, 600, 2000);

    // Insert some levels manually.
    book->updateBid(550, 10);
    book->updateAsk(590, 10);

    // Apply a snapshot with different levels.
    std::vector<std::pair<int64_t, uint64_t>> bids = {{700, 100}, {699, 200}};
    std::vector<std::pair<int64_t, uint64_t>> asks = {{800, 100}, {801, 200}};

    // Without reset, applySnapshot would just update existing levels.
    // But we want to test the normal flow: reset then apply.
    book->reset(700, 800, 2000);
    auto snap = book->snapshotActiveLevels();

    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());

    book->applySnapshot(bids, asks);

    EXPECT_EQ(book->bestBid(), 700);
    EXPECT_EQ(book->bestAsk(), 800);

    // Old levels (550, 590) should be gone.
    snap = book->snapshotActiveLevels();
    bool found_old = false;
    for (auto& b : snap.bids) {
        if (b.first == 550) found_old = true;
    }
    EXPECT_FALSE(found_old);

    // Also verify that the snapshot has the new levels.
    ASSERT_EQ(snap.bids.size(), 2);
    std::cout << "Bids (" << snap.bids.size() << "):\n";

    // for (const auto& bid : snap.bids) {
    //     std::cout << "price=" << bid.first
    //             << ", qty=" << bid.second << '\n';
    // }
    // std::cout << "Asks (" << snap.asks.size() << "):\n";
    // for (const auto& [price, qty] : snap.asks) {
    //     std::cout << "  " << price << " -> " << qty << '\n';
    // }

EXPECT_EQ(snap.bids[0].first, 700);
    EXPECT_EQ(snap.bids[1].first, 699);
}

// -----------------------------------------------------------------------------
// 5. applySnapshot with empty vectors
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, ApplySnapshotEmpty) {
    book->reset(500, 600, 2000);

    std::vector<std::pair<int64_t, uint64_t>> empty_bids;
    std::vector<std::pair<int64_t, uint64_t>> empty_asks;

    // Applying empty should not change the book.
    book->applySnapshot(empty_bids, empty_asks);
    // Best bid/ask remain as set in reset.
    EXPECT_EQ(book->bestBid(), -1);
    EXPECT_EQ(book->bestAsk(), -1);

    auto snap = book->snapshotActiveLevels();
    // The book is empty after reset, so snapshots should be empty.
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());

    // Now add levels and apply an empty snapshot (should not remove them).
    book->updateBid(550, 10);
    book->updateAsk(550, 10);
    book->applySnapshot(empty_bids, empty_asks);
    EXPECT_EQ(book->bestBid(), 550);
    EXPECT_EQ(book->bestAsk(), 550);

    // Applying a snapshot does NOT delete existing levels unless they are updated with 0.
    // So empty snapshot leaves book unchanged.
}

// -----------------------------------------------------------------------------
// 6. snapshotActiveLevels returns sorted levels
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, SnapshotActiveLevelsSorting) {
    book->reset(0, 0, 2000);

    // Insert bids in arbitrary order.
    book->updateBid(100, 10);
    book->updateBid(50, 20);
    book->updateBid(75, 30);

    // Insert asks in arbitrary order.
    book->updateAsk(200, 10);
    book->updateAsk(150, 20);
    book->updateAsk(175, 30);

    auto snap = book->snapshotActiveLevels();

    // Bids should be descending.
    ASSERT_EQ(snap.bids.size(), 3);
    EXPECT_EQ(snap.bids[0].first, 100);
    EXPECT_EQ(snap.bids[1].first, 75);
    EXPECT_EQ(snap.bids[2].first, 50);

    // Asks should be ascending.
    ASSERT_EQ(snap.asks.size(), 3);
    EXPECT_EQ(snap.asks[0].first, 150);
    EXPECT_EQ(snap.asks[1].first, 175);
    EXPECT_EQ(snap.asks[2].first, 200);
}

// -----------------------------------------------------------------------------
// 7. Mixed updates and snapshot
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, MixedUpdatesAndSnapshot) {
    book->reset(500, 600, 2000);

    // Apply initial snapshot.
    std::vector<std::pair<int64_t, uint64_t>> bids = {{500, 10}, {499, 20}};
    std::vector<std::pair<int64_t, uint64_t>> asks = {{600, 10}, {601, 20}};
    book->applySnapshot(bids, asks);
    EXPECT_EQ(book->bestBid(), 500);
    EXPECT_EQ(book->bestAsk(), 600);

    // Apply individual updates.
    book->updateBid(510, 30);   // new best bid
    book->updateAsk(590, 30);   // new best ask
    book->updateBid(499, 0);    // delete existing bid

    // Verify final state.
    EXPECT_EQ(book->bestBid(), 510);
    EXPECT_EQ(book->bestAsk(), 590);

    auto snap = book->snapshotActiveLevels();
    ASSERT_EQ(snap.bids.size(), 2); // 510 and 500 (499 deleted)
    EXPECT_EQ(snap.bids[0].first, 510);
    EXPECT_EQ(snap.bids[1].first, 500);

    ASSERT_EQ(snap.asks.size(), 3); // 590, 600, 601
    EXPECT_EQ(snap.asks[0].first, 590);
    EXPECT_EQ(snap.asks[1].first, 600);
    EXPECT_EQ(snap.asks[2].first, 601);
}

// -----------------------------------------------------------------------------
// 8. UpdateLevel triggers recalcBest
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, RecalcBestOnUpdate) {
    book->reset(500, 600, 2000);

    book->updateBid(550, 10);
    EXPECT_EQ(book->bestBid(), 550);

    // Delete best bid – should trigger recalc and find next best.
    book->updateBid(550, 0);
    // Since there are no other bids, best should revert to the reset value (500)
    // but note: reset set best_bid_tick=500, but the level 500 was never inserted.
    // The book's best_abs_tick is set to 500 on reset, but the bitmap doesn't have it.
    // updateBid(550, 0) deletes it, recalcBest scans the bitmap and finds nothing -> -1.
    // Wait, in reset we set best_abs_tick directly, but the level is not actually active.
    // This is a known design: reset sets the best tick, but the bitmap is empty.
    // The validator uses this to know the initial best tick.
    // Since we never inserted 500, it should be -1 after deletion.
    EXPECT_EQ(book->bestBid(), -1);
}

// -----------------------------------------------------------------------------
// 9. UpdateLevel deletes out-of-window (should not crash)
// -----------------------------------------------------------------------------

TEST_F(OrderBookTest, DeleteOutOfWindow) {
    book->reset(500, 600, 2000);
    // Window base is roughly mid - 1000 = 550-1000 = -450, size 2000 => end 1550.
    // 2000 is out of bounds.
    // Should not crash, just log or ignore.
    EXPECT_NO_THROW(book->updateBid(2000, 0));
}

TEST_F(OrderBookTest, RecenterPreservesExistingLevels)
{
    book->reset(1000, 1100, 100);

    book->updateBid(990, 10);
    book->updateBid(995, 20);
    book->updateBid(1000, 30);

    // Force a recenter far away.
    book->updateBid(5000, 50);

    auto snap = book->snapshotActiveLevels();

    ASSERT_EQ(snap.bids.size(), 4);

    EXPECT_EQ(snap.bids[0].first, 5000);
    EXPECT_EQ(snap.bids[0].second, 50);

    EXPECT_EQ(snap.bids[1].first, 1000);
    EXPECT_EQ(snap.bids[1].second, 30);

    EXPECT_EQ(snap.bids[2].first, 995);
    EXPECT_EQ(snap.bids[2].second, 20);

    EXPECT_EQ(snap.bids[3].first, 990);
    EXPECT_EQ(snap.bids[3].second, 10);
}

TEST_F(OrderBookTest, RecenterPreservesAskLevels)
{
    book->reset(1000, 1100, 100);

    book->updateAsk(1100, 10);
    book->updateAsk(1105, 20);
    book->updateAsk(1110, 30);

    // Force recenter.
    book->updateAsk(5000, 50);

    auto snap = book->snapshotActiveLevels();

    ASSERT_EQ(snap.asks.size(), 4);

    EXPECT_EQ(snap.asks[0].first, 1100);
    EXPECT_EQ(snap.asks[0].second, 10);

    EXPECT_EQ(snap.asks[1].first, 1105);
    EXPECT_EQ(snap.asks[1].second, 20);

    EXPECT_EQ(snap.asks[2].first, 1110);
    EXPECT_EQ(snap.asks[2].second, 30);

    EXPECT_EQ(snap.asks[3].first, 5000);
    EXPECT_EQ(snap.asks[3].second, 50);
}

TEST_F(OrderBookTest, RepeatedRecenterPreservesState) {
    book->reset(1000, 1100, 100);

    book->updateBid(1000, 10);
    book->updateBid(2000, 20);
    book->updateBid(3000, 30);
    book->updateBid(4000, 40);

    auto snap = book->snapshotActiveLevels();

    ASSERT_EQ(snap.bids.size(), 4);

    // Build O(1) lookup map from the snapshot.
    std::unordered_map<int64_t, uint64_t> bid_map;
    for (const auto& [price, qty] : snap.bids) {
        bid_map[price] = qty;
    }

    EXPECT_EQ(bid_map[1000], 10);
    EXPECT_EQ(bid_map[2000], 20);
    EXPECT_EQ(bid_map[3000], 30);
    EXPECT_EQ(bid_map[4000], 40);
}