#include "engine/apply_delta.hpp"

void applyDelta(const Delta& delta, OrderBook& book) {
    for (const auto& bid : delta.bids) {
        book.updateBid(bid.tick, bid.qty);
    }
    for (const auto& ask : delta.asks) {
        book.updateAsk(ask.tick, ask.qty);
    }
}