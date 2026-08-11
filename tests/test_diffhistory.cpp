#include <gtest/gtest.h>
#include "engine/diff_history_buffer.hpp"
#include "engine/delta.hpp"

// -----------------------------------------------------------------------------
// Fixture with a small capacity (16) to exercise wraparound.
// -----------------------------------------------------------------------------
class DiffHistoryBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer = std::make_unique<DiffHistoryBuffer>(16);
    }

    // Helper to create a Delta with given U,u and dummy bids/asks.
    Delta makeDelta(uint64_t U, uint64_t u) {
        Delta d;
        d.U = U;
        d.u = u;
        // Add a dummy bid level so the delta isn't empty.
        d.bids.emplace_back(1000, 10);
        d.asks.emplace_back(1001, 10);
        return d;
    }

    std::unique_ptr<DiffHistoryBuffer> buffer;
};

// -----------------------------------------------------------------------------
// 1. Empty buffer: getRange returns false.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, EmptyBuffer) {
    std::vector<Delta> out;
    EXPECT_FALSE(buffer->getRange(100, 200, out));
    EXPECT_TRUE(out.empty());
}

// -----------------------------------------------------------------------------
// 2. Push single delta, getRange returns it.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, SingleDelta) {
    auto d = makeDelta(100, 150);
    buffer->push(std::move(d));

    std::vector<Delta> out;

    // 1. Exact range (100..150) -> true
    EXPECT_TRUE(buffer->getRange(100, 150, out));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].U, 100);
    EXPECT_EQ(out[0].u, 150);

    out.clear();

    // 2. Range inside delta but not ending at delta boundary -> false (overshoots)
    EXPECT_FALSE(buffer->getRange(110, 140, out));
    EXPECT_TRUE(out.empty());

    out.clear();

    // 3. Range that starts before delta and ends after -> false (gaps + overshoot)
    EXPECT_FALSE(buffer->getRange(90, 160, out));
    EXPECT_TRUE(out.empty());

    out.clear();

    // 4. Range after delta -> false
    EXPECT_FALSE(buffer->getRange(151, 200, out));
    EXPECT_TRUE(out.empty());
}
// -----------------------------------------------------------------------------
// 3. Overlapping events (Binance edge case)
//    Snapshot S=100, event U=95,u=105 covers S+1.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, OverlappingEvent) {
    // Event that covers 95..105
    auto d1 = makeDelta(95, 105);
    buffer->push(std::move(d1));

    std::vector<Delta> out;
    // S=100, to=110
    EXPECT_FALSE(buffer->getRange(100, 110, out));
    EXPECT_TRUE(out.empty());

    // Add a second event that chains
    auto d2 = makeDelta(106, 120);
    buffer->push(std::move(d2));
    out.clear();
    EXPECT_TRUE(buffer->getRange(100, 120, out));
    EXPECT_FALSE(out.empty());
    ASSERT_EQ(out.size(), 2); 
}

// -----------------------------------------------------------------------------
// 4. Chain continuity (no gaps)
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, ChainContinuity) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(151, 200);
    auto d3 = makeDelta(201, 250);
    buffer->push(std::move(d1));
    buffer->push(std::move(d2));
    buffer->push(std::move(d3));

    std::vector<Delta> out;
    EXPECT_TRUE(buffer->getRange(100, 250, out));
    ASSERT_EQ(out.size(), 3);
    EXPECT_EQ(out[0].u, 150);
    EXPECT_EQ(out[1].u, 200);
    EXPECT_EQ(out[2].u, 250);
}

// -----------------------------------------------------------------------------
// 5. Gap detection: missing event causes failure.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, GapDetection) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(200, 250); // gap 151..199
    buffer->push(std::move(d1));
    buffer->push(std::move(d2));

    std::vector<Delta> out;
    // Request range that includes the gap
    EXPECT_FALSE(buffer->getRange(100, 250, out));
    EXPECT_TRUE(out.empty());

    // Request range within first event
    EXPECT_TRUE(buffer->getRange(100, 150, out));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].u, 150);
}

// -----------------------------------------------------------------------------
// 6. Buffer wraparound: oldest_u_ updates correctly.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 6. Buffer wraparound: oldest entries are discarded correctly.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, Wraparound) {
    // Capacity = 16.
    //
    // Create 20 contiguous events:
    //
    // [0,9], [10,19], ..., [190,199]
    //
    // After 20 pushes, the first 4 events must have been overwritten.
    for (int i = 0; i < 20; ++i) {
        auto d = makeDelta(i * 10, i * 10 + 9);
        buffer->push(std::move(d));
    }

    // Retained events should start at [40,49].
    EXPECT_EQ(buffer->oldestRetainedU(), 49);
    
    // The old [0,9] range is no longer retained.
    std::vector<Delta> out;

    EXPECT_FALSE(buffer->getRange(0, 9, out));
    EXPECT_TRUE(out.empty());

    // [40,49] -> [50,59] -> [60,69] is fully retained and contiguous.
    out.clear();

    EXPECT_TRUE(buffer->getRange(40, 69, out));
    ASSERT_EQ(out.size(), 3);

    EXPECT_EQ(out[0].U, 40);
    EXPECT_EQ(out[0].u, 49);

    EXPECT_EQ(out[1].U, 50);
    EXPECT_EQ(out[1].u, 59);

    EXPECT_EQ(out[2].U, 60);
    EXPECT_EQ(out[2].u, 69);

    // Latest retained range should also work.
    out.clear();

    EXPECT_TRUE(buffer->getRange(180, 199, out));
    ASSERT_EQ(out.size(), 2);

    EXPECT_EQ(out[0].U, 180);
    EXPECT_EQ(out[0].u, 189);

    EXPECT_EQ(out[1].U, 190);
    EXPECT_EQ(out[1].u, 199);
}

// -----------------------------------------------------------------------------
// 7. getRange returns false when range outside retained.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, RangeOutsideRetained) {
    auto d = makeDelta(100, 150);
    buffer->push(std::move(d));

    std::vector<Delta> out;
    // Range before oldest
    EXPECT_FALSE(buffer->getRange(0, 50, out));
    EXPECT_TRUE(out.empty());

    // Range after newest
    EXPECT_FALSE(buffer->getRange(200, 250, out));
    EXPECT_TRUE(out.empty());
}

// -----------------------------------------------------------------------------
// 8. Duplicate events: should be ignored.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, DuplicateEvents) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(100, 150); // duplicate
    buffer->push(std::move(d1));
    buffer->push(std::move(d2));

    std::vector<Delta> out;
    // getRange should include only one copy.
    EXPECT_TRUE(buffer->getRange(100, 150, out));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].u, 150);

    // Push a chain after duplicate
    auto d3 = makeDelta(151, 200);
    buffer->push(std::move(d3));
    out.clear();
    EXPECT_TRUE(buffer->getRange(100, 200, out));
    // Should still be 2 events (the duplicate skipped, chain continues)
    // But the duplicate has same U/u, so getRange chain logic will skip it.
    // Our getRange checks U <= seq+1, but for duplicate U=100, seq=150? Let's see:
    // After first event seq=150. Duplicate U=100 <= 151, but u=150 <= seq, so it skips.
    // Then d3 U=151 <= 151? Actually d3 U=151, seq=150, U<=151 yes, so includes.
    // So total should be 2.
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out[0].u, 150);
    EXPECT_EQ(out[1].u, 200);
}

// -----------------------------------------------------------------------------
// 9. getRange succeeds when the delta chain reaches exactly to_seq.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, ChainCoversToSeq) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(151, 180);
    auto d3 = makeDelta(181, 200);

    buffer->push(std::move(d1));
    buffer->push(std::move(d2));
    buffer->push(std::move(d3));

    std::vector<Delta> out;

    // Requested sequence range: 101..200.
    // Chain:
    //   [100,150] -> [151,180] -> [181,200]
    // reaches exactly 200.
    EXPECT_TRUE(buffer->getRange(100, 200, out));

    ASSERT_EQ(out.size(), 3);
    EXPECT_EQ(out[0].U, 100);
    EXPECT_EQ(out[0].u, 150);
    EXPECT_EQ(out[1].U, 151);
    EXPECT_EQ(out[1].u, 180);
    EXPECT_EQ(out[2].U, 181);
    EXPECT_EQ(out[2].u, 200);

    // Request ends inside d3.
    // d3 would take us from 180 -> 200, overshooting 190.
    // Therefore the requested range cannot be represented exactly.
    out.clear();

    EXPECT_FALSE(buffer->getRange(100, 190, out));
    EXPECT_TRUE(out.empty());
}


// -----------------------------------------------------------------------------
// 10. getRange fails when the target lies inside a delta.
// -----------------------------------------------------------------------------
TEST_F(DiffHistoryBufferTest, OvershootTargetFails) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(151, 200);

    buffer->push(std::move(d1));
    buffer->push(std::move(d2));

    std::vector<Delta> out;

    // Requested sequence range: 101..175.
    //
    // d1 reaches 150.
    // d2 starts at 151 but ends at 200.
    //
    // Since we cannot partially apply d2, it overshoots the requested
    // target 175. Therefore the request must fail.
    EXPECT_FALSE(buffer->getRange(100, 175, out));
    EXPECT_TRUE(out.empty());
}
TEST_F(DiffHistoryBufferTest, ExactTargetSucceeds) {
    auto d1 = makeDelta(100, 150);
    auto d2 = makeDelta(151, 175);  // ends exactly at to_seq

    buffer->push(std::move(d1));
    buffer->push(std::move(d2));

    std::vector<Delta> out;

    EXPECT_TRUE(buffer->getRange(100, 175, out));
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out[1].u, 175);
}