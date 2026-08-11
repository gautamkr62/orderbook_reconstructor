#include <gtest/gtest.h>
#include "orderbook/orderbook.hpp"
#include "engine/comparebooks.hpp"
#include <sstream>

class CompareBooksTest : public ::testing::Test {
protected:
    std::unique_ptr<OrderBook> buildBook(const std::vector<std::pair<int64_t, uint64_t>>& bids,
                                          const std::vector<std::pair<int64_t, uint64_t>>& asks) {
        int64_t best_bid = bids.empty() ? 0 : bids.front().first;
        int64_t best_ask = asks.empty() ? 0 : asks.front().first;
        auto book = std::make_unique<OrderBook>(best_bid, best_ask, 10000);
        book->applySnapshot(bids, asks);
        return book;
    }
};

TEST_F(CompareBooksTest, ExactMatch) {
    std::vector<std::pair<int64_t, uint64_t>> bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> asks = {{101, 10}, {102, 20}, {103, 30}};

    auto shadow = buildBook(bids, asks);
    std::string details;

    EXPECT_TRUE(compareBooks(*shadow, bids, asks, 3, 3, details));
}

TEST_F(CompareBooksTest, MissingPriceInShadow) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {{100, 10}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {{101, 10}, {102, 20}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_FALSE(compareBooks(*shadow, frozen_bids, frozen_asks, 3, 2, details));
    EXPECT_NE(details.find("Bid mismatch"), std::string::npos);
}

TEST_F(CompareBooksTest, QuantityMismatch) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {{100, 10}, {99, 25}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {{101, 10}, {102, 20}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_FALSE(compareBooks(*shadow, frozen_bids, frozen_asks, 3, 2, details));
    EXPECT_NE(details.find("mismatch"), std::string::npos);
}

TEST_F(CompareBooksTest, TailDeletionDoesNotFail) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {{100, 10}, {99, 20}};
    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {{101, 10}, {102, 20}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_TRUE(compareBooks(*shadow, frozen_bids, frozen_asks, 3, 2, details));
}

TEST_F(CompareBooksTest, InternalMismatchFails) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {{100, 10}, {99, 25}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {{101, 10}, {102, 20}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_FALSE(compareBooks(*shadow, frozen_bids, frozen_asks, 3, 2, details));
    EXPECT_NE(details.find("mismatch"), std::string::npos);
}

TEST_F(CompareBooksTest, AsymmetricalBidAskDepth) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {{100, 10}, {99, 20}, {98, 30}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = frozen_bids;
    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {{101, 10}, {102, 20}};
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_TRUE(compareBooks(*shadow, frozen_bids, frozen_asks, 3, 2, details));
}

TEST_F(CompareBooksTest, EmptyBooks) {
    std::vector<std::pair<int64_t, uint64_t>> empty;
    auto shadow = std::make_unique<OrderBook>(0, 0, 10000);
    std::string details;

    EXPECT_TRUE(compareBooks(*shadow, empty, empty, 0, 0, details));
}

TEST_F(CompareBooksTest, ExtraPriceInShadowFails) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {
        {100, 10}, {99, 20}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {
        {100, 10}, {99, 20}, {98, 30}
    };

    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {
        {101, 10}, {102, 20}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = frozen_asks;

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_TRUE(compareBooks(
        *shadow,
        frozen_bids, frozen_asks,
        2, 2,
        details
    ));
}

TEST_F(CompareBooksTest, ExtraAskFails) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {
        {100, 10}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = frozen_bids;

    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {
        {101, 10}, {102, 20}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = {
        {101, 10}, {102, 20}, {103, 30}
    };

    auto shadow = buildBook(shadow_bids, shadow_asks);
    std::string details;

    EXPECT_TRUE(compareBooks(
        *shadow,
        frozen_bids, frozen_asks,
        1, 2,
        details
    ));
}

TEST_F(CompareBooksTest, BestBidMismatch) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {
        {100, 10}, {99, 20}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {
        {101, 10}, {99, 20}
    };

    std::vector<std::pair<int64_t, uint64_t>> asks = {
        {102, 10}
    };

    auto shadow = buildBook(shadow_bids, asks);
    std::string details;

    EXPECT_FALSE(compareBooks(
        *shadow,
        frozen_bids, asks,
        2, 1,
        details
    ));
}


TEST_F(CompareBooksTest, BestAskMismatch) {
    std::vector<std::pair<int64_t, uint64_t>> bids = {
        {100, 10}
    };

    std::vector<std::pair<int64_t, uint64_t>> frozen_asks = {
        {101, 10}, {102, 20}
    };
    std::vector<std::pair<int64_t, uint64_t>> shadow_asks = {
        {100, 10}, {102, 20}
    };

    auto shadow = buildBook(bids, shadow_asks);
    std::string details;

    EXPECT_FALSE(compareBooks(
        *shadow,
        bids, frozen_asks,
        1, 2,
        details
    ));
}


TEST_F(CompareBooksTest, ZeroQuantityMismatch) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {
        {100, 10}, {99, 20}
    };

    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {
        {100, 10}, {99, 0}
    };

    std::vector<std::pair<int64_t, uint64_t>> asks = {
        {101, 10}
    };

    auto shadow = buildBook(shadow_bids, asks);
    std::string details;

    EXPECT_TRUE(compareBooks(
        *shadow,
        frozen_bids, asks,
        2, 1,
        details
    ));
}


TEST_F(CompareBooksTest, NonEmptyBidsEmptyAsks) {
    std::vector<std::pair<int64_t, uint64_t>> bids = {
        {100, 10}, {99, 20}
    };

    std::vector<std::pair<int64_t, uint64_t>> asks;

    auto shadow = buildBook(bids, asks);
    std::string details;

    EXPECT_TRUE(compareBooks(
        *shadow,
        bids, asks,
        2, 0,
        details
    ));
}


TEST_F(CompareBooksTest, OrderingMismatch) {
    std::vector<std::pair<int64_t, uint64_t>> frozen_bids = {
        {100, 10}, {99, 20}, {98, 30}
    };

    std::vector<std::pair<int64_t, uint64_t>> shadow_bids = {
        {99, 20}, {100, 10}, {98, 30}
    };

    std::vector<std::pair<int64_t, uint64_t>> asks = {
        {101, 10}
    };

    auto shadow = buildBook(shadow_bids, asks);
    std::string details;

    EXPECT_TRUE(compareBooks(
        *shadow,
        frozen_bids, asks,
        3, 1,
        details
    ));
}

TEST_F(CompareBooksTest, LargeBookSingleDeepMismatch) {
    constexpr size_t N = 1000;

    std::vector<std::pair<int64_t, uint64_t>> bids;
    std::vector<std::pair<int64_t, uint64_t>> asks;

    for (size_t i = 0; i < N; ++i) {
        bids.emplace_back(10000 - i, 100 + i);
        asks.emplace_back(11000 + i, 200 + i);
    }

    auto shadow = buildBook(bids, asks);

    // Corrupt one deep level.
    auto corrupted = bids;
    corrupted[750].second++;

    std::string details;

    EXPECT_FALSE(compareBooks(
        *shadow,
        corrupted, asks,
        N, N,
        details
    ));
}