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
    return snapshotActiveLevelsLocked();
}

// std::shared_mutex is not recursive: callers that already hold mutex_ (e.g.
// Validator::freeze) must use this, or libc++ deadlocks behind a waiting writer.
OrderBook::ActiveSnapshot OrderBook::snapshotActiveLevelsLocked() const {
    ActiveSnapshot snap;
    snap.bids.reserve(8192);
    snap.asks.reserve(8192);

    // The summary bitmap has one bit per non-empty bitmap word, so both loops
    // visit only words that hold active levels: cost ~ active levels, not
    // window size (a 20M-slot BTC window is 312K bitmap words per side).

    // ---- BIDS: best bid first (descending price) ----
    const auto& bw = bids.getWindow();
    for (size_t sw = bw.summary.size(); sw-- > 0;) {
        uint64_t sword = bw.summary[sw];
        while (sword) {
            const int sbit = 63 - __builtin_clzll(sword);
            sword &= ~(1ULL << sbit);
            const size_t w = (sw << 6) + static_cast<size_t>(sbit);
            if (w >= bw.bitmap.size()) continue;
            uint64_t word = bw.bitmap[w];
            while (word) {
                const int bit = 63 - __builtin_clzll(word);
                word &= ~(1ULL << bit);
                const size_t idx = (w << 6) + static_cast<size_t>(bit);
                if (idx < bw.levels.size())
                    snap.bids.emplace_back(bw.base_tick + static_cast<int64_t>(idx), bw.levels[idx].quantity);
            }
        }
    }

    // ---- ASKS: best ask first (ascending price) ----
    const auto& aw = asks.getWindow();
    for (size_t sw = 0; sw < aw.summary.size(); ++sw) {
        uint64_t sword = aw.summary[sw];
        while (sword) {
            const int sbit = __builtin_ctzll(sword);
            sword &= sword - 1;
            const size_t w = (sw << 6) + static_cast<size_t>(sbit);
            if (w >= aw.bitmap.size()) continue;
            uint64_t word = aw.bitmap[w];
            while (word) {
                const int bit = __builtin_ctzll(word);
                word &= word - 1;
                const size_t idx = (w << 6) + static_cast<size_t>(bit);
                if (idx < aw.levels.size())
                    snap.asks.emplace_back(aw.base_tick + static_cast<int64_t>(idx), aw.levels[idx].quantity);
            }
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

