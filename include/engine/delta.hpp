#pragma once

#include <cstdint>
#include <vector>

/**
 * LevelUpdate – A single price level change.
 *
 * More expressive than std::pair<int64_t, uint64_t>.
 */
struct LevelUpdate {
    int64_t tick;     // Price as scaled integer (tick)
    uint64_t qty;     // Quantity as scaled integer

    // Convenience constructor
    LevelUpdate(int64_t tick_, uint64_t qty_) : tick(tick_), qty(qty_) {}

    // Default constructor (for vector resize)
    LevelUpdate() : tick(0), qty(0) {}
};

/**
 * Delta – Fully parsed Binance depth delta.
 *
 * This is the shared data structure passed between the Reconciler (producer)
 * and the Validator (consumer via the DiffHistoryBuffer).
 */
struct Delta {
    uint64_t U; // First update ID in this delta
    uint64_t u; // Last update ID in this delta

    // Bids: vector of LevelUpdate
    std::vector<LevelUpdate> bids;

    // Asks: vector of LevelUpdate
    std::vector<LevelUpdate> asks;

    // Default constructor
    Delta() : U(0), u(0) {}

    // Convenience constructor
    Delta(uint64_t U_, uint64_t u_,
          std::vector<LevelUpdate> bids_,
          std::vector<LevelUpdate> asks_)
        : U(U_), u(u_), bids(std::move(bids_)), asks(std::move(asks_)) {}

    // Move constructor (default)
    Delta(Delta&&) = default;
    Delta& operator=(Delta&&) = default;

    // FIX #1: Make copyable for the Validator (cold path, once per minute).
    Delta(const Delta&) = default;
    Delta& operator=(const Delta&) = default;
};