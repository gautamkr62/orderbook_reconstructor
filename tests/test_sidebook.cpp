#include <gtest/gtest.h>
#include <map>
#include <random>
#include <set>
#include "orderbook/side_book.hpp"

// -----------------------------------------------------------------------------
// Fixture for BidSide
// -----------------------------------------------------------------------------
class BidSideTest : public ::testing::Test {
protected:
    void SetUp() override {
        book.init(1000, 2000);  // mid_tick=1000, window_size=2000 => base=0
    }

    BidSide book;
};

// -----------------------------------------------------------------------------
// Fixture for AskSide
// -----------------------------------------------------------------------------
class AskSideTest : public ::testing::Test {
protected:
    void SetUp() override {
        book.init(1000, 2000);
    }

    AskSide book;
};

// -----------------------------------------------------------------------------
// Test: init sets up window correctly
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, InitSetsWindow) {
    EXPECT_EQ(book.bestAbsTick(), -1);
    const auto& win = book.getWindow();
    EXPECT_EQ(win.base_tick, 0);
    EXPECT_EQ(win.size, 2000);
    EXPECT_EQ(win.levels.size(), 2000);
    EXPECT_EQ(win.bitmap.size(), (2000 >> 6) + 1);
}

// -----------------------------------------------------------------------------
// Test: Insert and update best tick
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, InsertLevel) {
    book.updateLevel(950, 100);
    EXPECT_EQ(book.bestAbsTick(), 950);
    book.updateLevel(960, 50);
    EXPECT_EQ(book.bestAbsTick(), 960);
    book.updateLevel(940, 200);
    EXPECT_EQ(book.bestAbsTick(), 960);
}

// -----------------------------------------------------------------------------
// Test: Delete level and best tick updates
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, DeleteLevel) {
    book.updateLevel(950, 100);
    book.updateLevel(960, 50);
    book.updateLevel(940, 200);

    book.updateLevel(960, 0);
    EXPECT_EQ(book.bestAbsTick(), 950);

    book.updateLevel(950, 0);
    book.updateLevel(940, 0);
    EXPECT_EQ(book.bestAbsTick(), -1);
}

TEST_F(BidSideTest, DeleteClearsBitmapAndLevel) {
    book.updateLevel(1200, 100);

    const auto& win1 = book.getWindow();
    int64_t rel = 1200 - win1.base_tick;

    ASSERT_NE(win1.levels[rel].quantity, 0);

    book.updateLevel(1200, 0);

    const auto& win2 = book.getWindow();
    rel = 1200 - win2.base_tick;

    EXPECT_EQ(win2.levels[rel].quantity, 0);

    bool bit_set =
        win2.bitmap[rel >> 6] &
        (1ULL << (rel & 63));

    EXPECT_FALSE(bit_set);
}


TEST_F(BidSideTest, EmptyInsertDeleteCycle) {
    EXPECT_EQ(book.bestAbsTick(), -1);

    book.updateLevel(1200, 100);
    EXPECT_EQ(book.bestAbsTick(), 1200);

    book.updateLevel(1200, 0);
    EXPECT_EQ(book.bestAbsTick(), -1);
}

// -----------------------------------------------------------------------------
// Test: Update existing level (overwrite)
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, UpdateLevel) {
    book.updateLevel(950, 100);
    EXPECT_EQ(book.bestAbsTick(), 950);
    book.updateLevel(950, 200);
    EXPECT_EQ(book.bestAbsTick(), 950);

    const auto& win = book.getWindow();
    int64_t rel_idx = 950 - win.base_tick;
    EXPECT_EQ(win.levels[rel_idx].quantity, 200);
    bool bit_set = (win.bitmap[rel_idx >> 6] & (1ULL << (rel_idx & 63))) != 0;
    EXPECT_TRUE(bit_set);
}

// -----------------------------------------------------------------------------
// Test: isInBounds and shouldRecenter
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, BoundsAndRecenter) {
    EXPECT_TRUE(book.isInBounds(500));
    EXPECT_TRUE(book.isInBounds(1999));
    EXPECT_FALSE(book.isInBounds(-1));
    EXPECT_FALSE(book.isInBounds(2000));

    EXPECT_FALSE(book.shouldRecenter(100));
    EXPECT_TRUE(book.shouldRecenter(1900));
}

// -----------------------------------------------------------------------------
// Test: Recenter preserves all active levels (with detailed checks)
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, RecenterPreservesLevels) {
    std::vector<int64_t> ticks = {200, 500, 800, 1200, 1500};
    for (auto t : ticks) {
        book.updateLevel(t, 100);
    }

    book.updateLevel(1900, 50);

    const auto& win = book.getWindow();

    for (auto t : ticks) {
        int64_t rel = t - win.base_tick;
        ASSERT_GE(rel, 0);
        ASSERT_LT(rel, static_cast<int64_t>(win.size));
        EXPECT_EQ(win.levels[rel].quantity, 100);
        bool bit_set = (win.bitmap[rel >> 6] & (1ULL << (rel & 63))) != 0;
        EXPECT_TRUE(bit_set) << "Bitmap bit not set for tick " << t;
    }

    int64_t rel_trigger = 1900 - win.base_tick;
    EXPECT_EQ(win.levels[rel_trigger].quantity, 50);
    bool bit_trigger = (win.bitmap[rel_trigger >> 6] & (1ULL << (rel_trigger & 63))) != 0;
    EXPECT_TRUE(bit_trigger);
    EXPECT_EQ(book.bestAbsTick(), 1900);
}

TEST_F(BidSideTest, RecenterDownwardPreservesLevels) {
    book.updateLevel(1000, 10);
    book.updateLevel(1500, 20);
    book.updateLevel(1900, 30);

    book.updateLevel(100, 40);

    const auto& win = book.getWindow();

    for (auto tick : {100, 1000, 1500, 1900}) {
        int64_t rel = tick - win.base_tick;

        ASSERT_GE(rel, 0);
        ASSERT_LT(rel, static_cast<int64_t>(win.size));

        EXPECT_NE(win.levels[rel].quantity, 0);
        EXPECT_TRUE(
            win.bitmap[rel >> 6] &
            (1ULL << (rel & 63))
        );
    }
}


TEST_F(BidSideTest, DeleteAfterRecenter) {
    book.updateLevel(1000, 10);
    book.updateLevel(1900, 20);

    book.updateLevel(50, 30); // forces recenter

    book.updateLevel(1900, 0);

    EXPECT_NE(book.bestAbsTick(), 1900);
}

// -----------------------------------------------------------------------------
// Test: Reference model with fixed seed and wide tick range
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, ReferenceModel) {
    std::map<int64_t, uint64_t> ref;
    std::mt19937 gen(12345);
    std::uniform_int_distribution<int64_t> tick_dist(0, 100000);
    std::uniform_int_distribution<uint64_t> qty_dist(0, 1000);
    std::uniform_int_distribution<int> op_dist(0, 2);

    const int NUM_OPS = 10000;
    for (int i = 0; i < NUM_OPS; ++i) {
        int64_t tick = tick_dist(gen);
        uint64_t qty = qty_dist(gen);
        int op = op_dist(gen);

        if (op == 2) qty = 0;

        // Update reference
        if (qty == 0) {
            ref.erase(tick);
        } else {
            ref[tick] = qty;
        }

        // Update SideBook
        book.updateLevel(tick, qty);

        // Verify consistency
        const auto& win = book.getWindow();

        // Check quantity and bitmap for each reference level
        for (const auto& [t, q] : ref) {
            int64_t rel = t - win.base_tick;
            ASSERT_GE(rel, 0) << "Reference tick " << t << " fell outside window (below base)";
            ASSERT_LT(rel, static_cast<int64_t>(win.size)) << "Reference tick " << t << " fell outside window (above size)";
            EXPECT_EQ(win.levels[rel].quantity, q) << "Quantity mismatch for tick " << t;
            bool bit_set = (win.bitmap[rel >> 6] & (1ULL << (rel & 63))) != 0;
            EXPECT_TRUE(bit_set) << "Bitmap bit not set for tick " << t;
        }

        // Check best tick against reference
        int64_t expected_best = -1;
        if (!ref.empty()) {
            expected_best = ref.rbegin()->first;
        }
        EXPECT_EQ(book.bestAbsTick(), expected_best) << "Best tick mismatch after operation " << i;

        // Count active levels
        size_t active_count = 0;
        for (size_t word_idx = 0; word_idx < win.bitmap.size(); ++word_idx) {
            uint64_t word = win.bitmap[word_idx];
            active_count += __builtin_popcountll(word);
        }
        EXPECT_EQ(active_count, ref.size()) << "Active level count mismatch after operation " << i;
    }
}

// -----------------------------------------------------------------------------
// AskSide tests
// -----------------------------------------------------------------------------
TEST_F(AskSideTest, InsertAndDelete) {
    book.updateLevel(950, 100);
    EXPECT_EQ(book.bestAbsTick(), 950);
    book.updateLevel(940, 50);
    EXPECT_EQ(book.bestAbsTick(), 940);
    book.updateLevel(940, 0);
    EXPECT_EQ(book.bestAbsTick(), 950);
}

TEST_F(AskSideTest, AskOrdering) {
    book.updateLevel(1000, 10);
    book.updateLevel(999, 20);
    book.updateLevel(1001, 30);
    EXPECT_EQ(book.bestAbsTick(), 999);
}

// -----------------------------------------------------------------------------
// Test: Consistency check (bitmap ↔ levels) for BidSide
// -----------------------------------------------------------------------------
TEST_F(BidSideTest, ConsistencyCheck) {
    book.updateLevel(100, 10);
    book.updateLevel(200, 20);
    book.updateLevel(300, 30);

    const auto& win = book.getWindow();
    for (size_t idx = 0; idx < win.levels.size(); ++idx) {
        bool active = (win.levels[idx].quantity != 0);
        bool bit_set = (win.bitmap[idx >> 6] & (1ULL << (idx & 63))) != 0;
        EXPECT_EQ(active, bit_set) << "Inconsistency at idx " << idx;
    }
}