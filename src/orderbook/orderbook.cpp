#include "orderbook/orderbook.hpp"
#include<orderbook/side_book.hpp>
#include<algorithm> 
// ---------------------------------------------------------------------
// Constructor
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

void OrderBook::applySnapshot(const std::vector<std::pair<int64_t, uint64_t>>& bids_data,
                              const std::vector<std::pair<int64_t, uint64_t>>& asks_data) {
    // std::cout << "[SNAPSHOT][" << label_ << "] Applying " << bids_data.size() << " bids and " << asks_data.size() << " asks" << std::endl;

    
    std::cout << "[SNAPSHOT] Applying " << bids_data.size() << " bids, " << asks_data.size() << " asks" << std::endl;

    for (const auto& [tick, qty] : bids_data) {
        updateBid(tick, qty);
    }

    for (const auto& [tick, qty] : asks_data) {
        updateAsk(tick, qty);
    }

    bids.recalcBest();
    asks.recalcBest();
}


// ---------------------------------------------------------------------
// Delta Updates (Simple forwarding)
// ---------------------------------------------------------------------

void OrderBook::updateBid(int64_t abs_tick, uint64_t quantity) {
    // std::cout << "[Bid] tick=" << abs_tick
            // << " qty=" << quantity << '\n';

    bids.updateLevel(abs_tick, quantity);
}

void OrderBook::updateAsk(int64_t abs_tick, uint64_t quantity) {
    // std::cout << "[Ask] tick=" << abs_tick
        // << " qty=" << quantity << '\n';

    asks.updateLevel(abs_tick, quantity);
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
    // Capture bids
    // const auto& bid_win = bids.window;
    const auto& bid_win = bids.getWindow();

    for (size_t idx = 0; idx < bid_win.levels.size(); ++idx) {
        if (bid_win.levels[idx].quantity != 0) {
            int64_t tick = bid_win.base_tick + static_cast<int64_t>(idx);
            snap.bids.emplace_back(tick, bid_win.levels[idx].quantity);
        }
    }
    // Sort descending (best bid first)
    std::sort(snap.bids.begin(), snap.bids.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Capture asks
    const auto& ask_win = asks.getWindow();
    for (size_t idx = 0; idx < ask_win.levels.size(); ++idx) {
        if (ask_win.levels[idx].quantity != 0) {
            int64_t tick = ask_win.base_tick + static_cast<int64_t>(idx);
            snap.asks.emplace_back(tick, ask_win.levels[idx].quantity);
        }
    }
    // Sort ascending (best ask first)
    std::sort(snap.asks.begin(), snap.asks.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

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

