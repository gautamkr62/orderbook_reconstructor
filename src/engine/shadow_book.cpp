#include "engine/shadow_book.hpp"
#include "orderbook/orderbook.hpp"

#include <algorithm>
#include <functional>

namespace {

// Overwrite / delete one level in a side sorted by `before` (best first).
template <typename Before>
void setLevel(ShadowBook::Levels& side, int64_t tick, uint64_t qty, Before before) {
    auto it = std::lower_bound(side.begin(), side.end(), tick,
                               [&](const auto& lvl, int64_t t) { return before(lvl.first, t); });
    const bool found = it != side.end() && it->first == tick;
    if (qty == 0) {
        if (found) side.erase(it);
    } else if (found) {
        it->second = qty;
    } else {
        side.insert(it, {tick, qty});
    }
}

} // namespace

void ShadowBook::applySnapshot(const Levels& bids, const Levels& asks) {
    auto load = [](const Levels& src, Levels& dst, auto before) {
        dst.clear();
        for (const auto& lvl : src)
            if (lvl.second != 0) dst.push_back(lvl);
        std::sort(dst.begin(), dst.end(),
                  [&](const auto& a, const auto& b) { return before(a.first, b.first); });
    };
    load(bids, bids_, std::greater<int64_t>());
    load(asks, asks_, std::less<int64_t>());
}

// Same rule as OrderBook::updateBid/updateAsk: a non-zero update further than
// MAX_DEPTH_RATIO from the current best is ignored.
void ShadowBook::updateBid(int64_t tick, uint64_t qty) {
    if (qty > 0 && !bids_.empty()) {
        const double best = static_cast<double>(bids_.front().first);
        if ((best - static_cast<double>(tick)) / best > OrderBook::MAX_DEPTH_RATIO) return;
    }
    setLevel(bids_, tick, qty, std::greater<int64_t>());
}

void ShadowBook::updateAsk(int64_t tick, uint64_t qty) {
    if (qty > 0 && !asks_.empty()) {
        const double best = static_cast<double>(asks_.front().first);
        if ((static_cast<double>(tick) - best) / best > OrderBook::MAX_DEPTH_RATIO) return;
    }
    setLevel(asks_, tick, qty, std::less<int64_t>());
}

void ShadowBook::apply(const Delta& delta) {
    for (const auto& lvl : delta.bids) updateBid(lvl.tick, lvl.qty);
    for (const auto& lvl : delta.asks) updateAsk(lvl.tick, lvl.qty);
}
