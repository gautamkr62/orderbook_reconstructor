#pragma once

#include "engine/delta.hpp"

#include <cstdint>
#include <utility>
#include <vector>

/**
 * ShadowBook – the Validator's independent reconstruction of the book:
 * a REST snapshot plus the deltas replayed on top of it.
 *
 * It only ever holds what one snapshot returns (~5000 levels per side) and a
 * few replayed deltas, so it keeps each side as a sorted vector instead of a
 * full tick-indexed window like OrderBook (which is 320 MB for BTC).
 *
 * Update semantics mirror OrderBook exactly — overwrite, qty 0 deletes, and
 * the same MAX_DEPTH_RATIO filter — so live and shadow stay comparable.
 */
class ShadowBook {
public:
    using Levels = std::vector<std::pair<int64_t, uint64_t>>;

    // Replace the contents with a REST snapshot (zero quantities are skipped).
    void applySnapshot(const Levels& bids, const Levels& asks);

    void apply(const Delta& delta);
    void updateBid(int64_t tick, uint64_t qty);
    void updateAsk(int64_t tick, uint64_t qty);

    const Levels& bids() const { return bids_; }  // best (highest) first
    const Levels& asks() const { return asks_; }  // best (lowest) first

private:
    Levels bids_;
    Levels asks_;
};
