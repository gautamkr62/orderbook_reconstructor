#include "engine/apply_delta.hpp"

void applyDelta(const Delta& delta, OrderBook& book) {
    // const auto bid_start = std::chrono::steady_clock::now();

    for (const auto& [tick, qty] : delta.bids) {
        book.updateBid(tick, qty);
    }

    // const auto bid_end = std::chrono::steady_clock::now();

    // const auto ask_start = std::chrono::steady_clock::now();
    
    for (const auto& [tick, qty] : delta.asks) {
        book.updateAsk(tick, qty);
    }
}

    // const auto ask_end = std::chrono::steady_clock::now();

    // const auto bid_apply_us =
    //     std::chrono::duration_cast<std::chrono::microseconds>(
    //         bid_end - bid_start).count();

    // const auto ask_apply_us =
    //     std::chrono::duration_cast<std::chrono::microseconds>(
    //         ask_end - ask_start).count();

    // const auto total_apply_us = bid_apply_us + ask_apply_us;

    // if (total_apply_us > 50) {
    //     std::cout << "[SLOW-APPLY-applydelta]"
    //               << " bid=" << bid_apply_us << "us"
    //               << " ask=" << ask_apply_us << "us"
    //               << " bids=" << delta.bids.size()
    //               << " asks=" << delta.asks.size()
    //               << std::endl;
    // }