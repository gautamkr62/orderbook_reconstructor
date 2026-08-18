#pragma once

#include "orderbook/orderbook.hpp"
#include <string>
#include <vector>
#include <cstdint>

/**
 * SnapshotBoundary – carries the REST snapshot's authoritative price
 * boundaries and depths. These values must come directly from the
 * REST snapshot, never from the live book.
 */
struct SnapshotBoundary {
    uint64_t last_update_id = 0;

    // The worst (deepest) price returned by the REST snapshot for each side.
    // Bids are sorted descending, so worst_bid_price is the smallest price
    // included in the snapshot. Asks are sorted ascending, so worst_ask_price
    // is the largest price included.
    int64_t worst_bid_price = 0;
    int64_t worst_ask_price = 0;

    // Number of levels actually returned by the REST snapshot.
    size_t bid_depth = 0;
    size_t ask_depth = 0;
};

/**
 * Compare two order book snapshots (frozen live vs shadow) using
 * the price boundary from the REST snapshot. Only levels within the
 * snapshot's price range are compared. This eliminates false mismatches
 * caused by the live book having more depth than the snapshot.
 *
 * @param shadow          The shadow book (rebuilt from REST + replay).
 * @param live_bids       Frozen live book bids (vector of {price, qty}).
 * @param live_asks       Frozen live book asks.
 * @param snapshot        Authoritative REST snapshot boundary.
 * @param mismatch_details Output string describing the first mismatch found.
 * @return true if the comparable regions match exactly; false otherwise.
 */
bool compareBooks(
    const OrderBook& shadow,
    const std::vector<std::pair<int64_t, uint64_t>>& live_bids,
    const std::vector<std::pair<int64_t, uint64_t>>& live_asks,
    const SnapshotBoundary& snapshot,
    std::string& mismatch_details
);