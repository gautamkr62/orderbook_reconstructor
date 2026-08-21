#include "orderbook/orderbook.hpp"
#include<orderbook/side_book.hpp>
#include<algorithm> 
// ---------------------------------------------------------------------
// 
// ---------------------------------------------------------------------

OrderBook::OrderBook(int64_t best_bid_tick, int64_t best_ask_tick, size_t window_size, std::string label)
    : label_(std::move(label)) {
    int64_t mid_tick = (best_bid_tick + best_ask_tick) / 2;
    bids.init(mid_tick, window_size);
    asks.init(mid_tick, window_size);
}



// ---------------------------------------------------------------------
// applySnapshot
// ---------------------------------------------------------------------

// In orderbook.cpp — replace the existing applySnapshot implementation with this.

void OrderBook::applySnapshot(const std::vector<std::pair<int64_t, uint64_t>>& bids_data,
                              const std::vector<std::pair<int64_t, uint64_t>>& asks_data)
{
    // std::cout << "[SNAPSHOT] Applying " << bids_data.size()
    //           << " bids, " << asks_data.size() << " asks" << std::endl;

    // Each SideBook sees only its own side's ticks.
    // applySnapshot sizes the window from actual data — zero recenters.
    bids.applySnapshot(bids_data.data(), bids_data.size());
    asks.applySnapshot(asks_data.data(), asks_data.size());

    // recalcBest is called inside applySnapshot — no extra call needed here.
}

// ---------------------------------------------------------------------
// Delta Updates (Simple forwarding)
// ---------------------------------------------------------------------

void OrderBook::updateBid(int64_t tick, uint64_t qty) {
    if (qty > 0 && bids.bestAbsTick() != -1) {
        const double ratio = static_cast<double>(bids.bestAbsTick() - tick)
                           / static_cast<double>(bids.bestAbsTick());
        if (ratio > MAX_DEPTH_RATIO) {
            // SB_LOG("updateBid: rejecting deep tick=%lld (%.1f%% from best=%lld)",
            //        (long long)tick, ratio * 100.0, (long long)bids.bestAbsTick());
            return;
        }
    }
    bids.updateLevel(tick, qty);
}

void OrderBook::updateAsk(int64_t tick, uint64_t qty) {
    // std::cout << "[Ask] tick=" << abs_tick
        // << " qty=" << quantity << '\n';

    if(qty > 0 && asks.bestAbsTick() != -1){
        const double ratio = static_cast<double>(tick - asks.bestAbsTick() )
                           / static_cast<double>(asks.bestAbsTick());

        if (ratio > MAX_DEPTH_RATIO) {
            // SB_LOG("updateAsk: rejecting deep tick=%lld (%.1f%% from best=%lld)",
            //        (long long)tick, ratio * 100.0, (long long)asks.bestAbsTick());
            return;
        }
    }

    asks.updateLevel(tick, qty);
}

// ---------------------------------------------------------------------
// Read API
// ---------------------------------------------------------------------

int64_t OrderBook::bestBid() const {
    std::shared_lock lock(mutex_);
    return bids.bestAbsTick();
}

int64_t OrderBook::bestAsk() const {
    std::shared_lock lock(mutex_);
    return asks.bestAbsTick();
}

void OrderBook::reset(int64_t best_bid_tick, int64_t best_ask_tick, size_t window_size) {
    int64_t mid_tick = (best_bid_tick + best_ask_tick) / 2;
    bids.init(mid_tick, window_size);
    asks.init(mid_tick, window_size);
}



OrderBook::ActiveSnapshot OrderBook::snapshotActiveLevels() const {
    std::shared_lock lock(mutex_);

    ActiveSnapshot snap;

    // ---- BIDS: best bid first (descending price) ----
    const auto& bid_win = bids.getWindow();
    const size_t bid_words = bid_win.bitmap.size();

    // Iterate word indices from high to low
    for (int w = static_cast<int>(bid_words) - 1; w >= 0; --w) {
        uint64_t word = bid_win.bitmap[w];

        while (word) {
            // Extract highest set bit (gives largest relative index)
            int bit = 63 - __builtin_clzll(word);
            size_t idx = (static_cast<size_t>(w) << 6) + bit;

            if (idx < bid_win.levels.size()) {
                int64_t tick = bid_win.base_tick + static_cast<int64_t>(idx);
                snap.bids.emplace_back(tick, bid_win.levels[idx].quantity);
            }

            // Clear the highest bit we just processed
            word &= ~(1ULL << bit);
        }
    }

    // ---- ASKS: best ask first (ascending price) ----
    const auto& ask_win = asks.getWindow();
    const size_t ask_words = ask_win.bitmap.size();

    // Iterate word indices from low to high
    for (size_t w = 0; w < ask_words; ++w) {
        uint64_t word = ask_win.bitmap[w];

        while (word) {
            // Extract lowest set bit (gives smallest relative index)
            int bit = __builtin_ctzll(word);
            size_t idx = (w << 6) + bit;

            if (idx < ask_win.levels.size()) {
                int64_t tick = ask_win.base_tick + static_cast<int64_t>(idx);
                snap.asks.emplace_back(tick, ask_win.levels[idx].quantity);
            }

            // Clear the lowest bit we just processed
            word &= word - 1;
        }
    }

    return snap;
}


bool OrderBook::verifyConsistency() const {
    std::shared_lock lock(mutex_);
    return verifyConsistencyLocked();
}

bool OrderBook::verifyConsistencyLocked() const {
    bool ok = true;
    if (!bids.verifyConsistency()) ok = false;
    if (!asks.verifyConsistency()) ok = false;
    return ok;
}

