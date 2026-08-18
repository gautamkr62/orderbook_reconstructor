#include "engine/comparebooks.hpp"
#include "orderbook/orderbook.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace {

bool compareSide(
    const std::vector<std::pair<int64_t, uint64_t>>& live,
    const std::vector<std::pair<int64_t, uint64_t>>& shadow,
    int64_t snapshot_boundary,
    bool is_bid,
    const char* side_name,
    std::string& mismatch_details)
{
    assert(std::is_sorted(
        live.begin(),
        live.end(),
        [is_bid](const auto& a, const auto& b) {
            return is_bid
                ? a.first > b.first
                : a.first < b.first;
        }));

    assert(std::is_sorted(
        shadow.begin(),
        shadow.end(),
        [is_bid](const auto& a, const auto& b) {
            return is_bid
                ? a.first > b.first
                : a.first < b.first;
        }));

    const auto withinBoundary =
        [snapshot_boundary, is_bid](int64_t price) {
            return is_bid
                ? price >= snapshot_boundary
                : price <= snapshot_boundary;
        };

    size_t li = 0;
    size_t si = 0;

    while (true)
    {
        const bool live_valid =
            li < live.size() &&
            withinBoundary(live[li].first);

        const bool shadow_valid =
            si < shadow.size() &&
            withinBoundary(shadow[si].first);

        // Both books exhausted inside the comparable region.
        if (!live_valid && !shadow_valid)
            return true;

        // Live contains a level that shadow does not.
        if (live_valid && !shadow_valid)
        {
            std::ostringstream oss;

            oss << side_name
                << " mismatch: live has extra level "
                   "inside REST snapshot boundary"
                << " price=" << live[li].first
                << " qty=" << live[li].second;

            mismatch_details = oss.str();
            return false;
        }

        // Shadow contains a level that live does not.
        if (!live_valid && shadow_valid)
        {
            std::ostringstream oss;

            oss << side_name
                << " mismatch: shadow has extra level "
                   "inside REST snapshot boundary"
                << " price=" << shadow[si].first
                << " qty=" << shadow[si].second;

            mismatch_details = oss.str();
            return false;
        }

        const auto [live_price, live_qty] = live[li];
        const auto [shadow_price, shadow_qty] = shadow[si];

        // Price sets differ.
        if (live_price != shadow_price)
        {
            std::ostringstream oss;

            oss << side_name
                << " price mismatch"
                << " live_price=" << live_price
                << " live_qty=" << live_qty
                << " shadow_price=" << shadow_price
                << " shadow_qty=" << shadow_qty;

            mismatch_details = oss.str();
            return false;
        }

        // Same price, different quantity.
        if (live_qty != shadow_qty)
        {
            std::ostringstream oss;

            oss << side_name
                << " quantity mismatch"
                << " price=" << live_price
                << " live_qty=" << live_qty
                << " shadow_qty=" << shadow_qty;

            mismatch_details = oss.str();
            return false;
        }

        ++li;
        ++si;
    }
}

} // namespace

bool compareBooks(
    const OrderBook& shadow,
    const std::vector<std::pair<int64_t, uint64_t>>& live_bids,
    const std::vector<std::pair<int64_t, uint64_t>>& live_asks,
    const SnapshotBoundary& snapshot,
    std::string& mismatch_details)
{
    mismatch_details.clear();

    // ------------------------------------------------------------
    // Validate REST snapshot metadata before comparing anything.
    // ------------------------------------------------------------
    if (snapshot.bid_depth == 0 ||
        snapshot.ask_depth == 0)
    {
        mismatch_details =
            "Invalid REST snapshot: empty bid or ask side.";
        return false;
    }

    if (snapshot.worst_bid_price <= 0 ||
        snapshot.worst_ask_price <= 0)
    {
        mismatch_details =
            "Invalid REST snapshot: missing price boundary.";
        return false;
    }

    if (snapshot.worst_bid_price >=
        snapshot.worst_ask_price)
    {
        mismatch_details =
            "Invalid REST snapshot: bid/ask boundaries overlap.";
        return false;
    }

    // ------------------------------------------------------------
    // Take one consistent snapshot of the shadow book.
    // ------------------------------------------------------------
    const auto shadow_snapshot =
        shadow.snapshotActiveLevels();

    // ------------------------------------------------------------
    // Compare only the price region represented by the REST
    // snapshot. The boundaries come exclusively from REST metadata.
    // ------------------------------------------------------------
    if (!compareSide(
            live_bids,
            shadow_snapshot.bids,
            snapshot.worst_bid_price,
            true,
            "Bid",
            mismatch_details))
    {
        return false;
    }

    if (!compareSide(
            live_asks,
            shadow_snapshot.asks,
            snapshot.worst_ask_price,
            false,
            "Ask",
            mismatch_details))
    {
        return false;
    }

    return true;
}