#pragma once


#include <shared_mutex>
#include "orderbook/side_book.hpp"

/**
 * OrderBook
 *
 * Aggregates BidSide and AskSide for a single trading symbol.
 * Uses a tick-indexed vector + bitmap layout for O(1) updates.
 *
 * ============================================================================
 * THREADING MODEL
 * ============================================================================
 * This class is NOT internally thread-safe.
 *
 * Single-Writer: Only the Reconciliation Engine thread may call:
 *   - updateBid()
 *   - updateAsk()
 *   - applySnapshot()
 *
 * Multiple-Readers: External threads (e.g., Validator, API) may read
 * bestBid() / bestAsk() only. For full snapshot reads, use an external
 * shared_mutex at the OrderBook level.
 *
 * The IO thread never touches this class directly.
 * ============================================================================
 *
 * ============================================================================
 * INTERFACE DESIGN
 * ============================================================================
 * All methods accept PRE-PARSED numeric values.
 *
 * The OrderBook does NOT parse strings. It does NOT know about JSON,
 * WebSockets, or price_parser.hpp.
 *
 * Parsing of price strings → absolute ticks is the responsibility of the
 * Reconciler/Engine thread.
 * ============================================================================
 */
class OrderBook {
public:
    /**
     * Constructor.
     * @param best_bid_tick  Absolute tick of the best bid from the first REST snapshot
     * @param best_ask_tick  Absolute tick of the best ask from the first REST snapshot
     * @param window_size    Number of ticks per side (default 1,000,000)
     */
    OrderBook(int64_t best_bid_tick, int64_t best_ask_tick, size_t window_size = 1'000'000);

    // ---------------------------------------------------------------------
    // Snapshot Application (Called exactly once at startup)
    // ---------------------------------------------------------------------

    /**
     * Bulk-load the order book from a REST depth snapshot.
     * @param bids  Vector of {absolute_tick, quantity} pairs
     * @param asks  Vector of {absolute_tick, quantity} pairs
     */
    void applySnapshot(const std::vector<std::pair<int64_t, uint64_t>>& bids,
                       const std::vector<std::pair<int64_t, uint64_t>>& asks);

    // ---------------------------------------------------------------------
    // Delta Application (Called for every WebSocket diff)
    // ---------------------------------------------------------------------

    /**
     * Apply a single bid delta (OVERWRITE semantics).
     * @param abs_tick  Absolute tick value (price * tick_scale)
     * @param quantity  New quantity at this price (0 means delete)
     */
    void updateBid(int64_t abs_tick, uint64_t quantity);

    /**
     * Apply a single ask delta (OVERWRITE semantics).
     * @param abs_tick  Absolute tick value (price * tick_scale)
     * @param quantity  New quantity at this price (0 means delete)
     */
    void updateAsk(int64_t abs_tick, uint64_t quantity);

    // ---------------------------------------------------------------------
    // Read API (Thread-safe for readers if externally synchronized)
    // ---------------------------------------------------------------------

    int64_t bestBid() const;
    int64_t bestAsk() const;
    void reset(int64_t best_bid_tick, int64_t best_ask_tick, size_t window_size = 1'000'000);

    struct ActiveSnapshot {
        std::vector<std::pair<int64_t, uint64_t>> bids;
        std::vector<std::pair<int64_t, uint64_t>> asks;
    };
    std::shared_mutex& getMutex() const { return mutex_; }
    ActiveSnapshot snapshotActiveLevels() const;
    

    bool verifyConsistency() const {
        bool ok = true;
        if (!bids.verifyConsistency()) ok = false;
        if (!asks.verifyConsistency()) ok = false;
        return ok;
    }

    void dumpActiveLevels() const ;  




private:
    SideBook<Side::Bid> bids;
    SideBook<Side::Ask> asks;
    mutable std::shared_mutex mutex_;
};