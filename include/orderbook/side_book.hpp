#pragma once
#define DEBUG_RECENTER 1
#include "price_level.hpp"
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cassert>
#include <iostream>

enum class Side { Bid, Ask };

template <Side S>
class SideBook {
public:
    struct Window {
        int64_t base_tick = 0;
        size_t size = 0;
        std::vector<PriceLevel> levels;
        std::vector<uint64_t> bitmap;
    };

    static constexpr double HYSTERESIS_MARGIN_RATIO = 0.10;

    SideBook() = default;

    void init(int64_t mid_tick, size_t window_size) {
        window.size = window_size;
        int64_t half_window = static_cast<int64_t>(window_size / 2);
        window.base_tick = mid_tick - half_window;
        if (window.base_tick < 0) window.base_tick = 0;
        window.levels.resize(window.size);
        window.bitmap.resize((window.size >> 6) + 1, 0);
        best_abs_tick = -1;
    }

    void updateLevel(int64_t abs_tick, uint64_t quantity) {
        if (quantity == 0 && !isInBounds(abs_tick)) {
            std::cerr << "[DELETE OUT OF WINDOW] tick=" << abs_tick
                      << " base=" << window.base_tick
                      << " size=" << window.size
                      << " rel_idx would be " << (abs_tick - window.base_tick) << '\n';
        }

        bool out_of_bounds = !isInBounds(abs_tick);
        bool needs_recenter = shouldRecenter(abs_tick);

        if (out_of_bounds || needs_recenter) {
            std::cerr << "[RECENTER TRIGGER] tick=" << abs_tick
                      << " base=" << window.base_tick
                      << " size=" << window.size
                      << " rel=" << (abs_tick - window.base_tick)
                      << " out_of_bounds=" << out_of_bounds
                      << " should_recenter=" << needs_recenter << '\n';
            recenter(abs_tick);
            if (!isInBounds(abs_tick)) {
                std::cerr << "CRITICAL: Price still out of bounds after recenter! "
                          << "abs_tick=" << abs_tick
                          << ", base=" << window.base_tick
                          << ", size=" << window.size << std::endl;
                return;
            }
        }

        int64_t rel_idx = abs_tick - window.base_tick;
        if (rel_idx < 0 || rel_idx >= static_cast<int64_t>(window.size)) return;

        PriceLevel& level = window.levels[rel_idx];
        bool was_active = (level.quantity != 0);
        level.quantity = quantity;

        uint64_t mask = 1ULL << (rel_idx & 63);
        if (quantity == 0) {
            window.bitmap[rel_idx >> 6] &= ~mask;
        } else if (!was_active) {
            window.bitmap[rel_idx >> 6] |= mask;
        }

        if (quantity == 0 && abs_tick == best_abs_tick) {
            recalcBest();
        } else if (quantity > 0) {
            if constexpr (S == Side::Bid) {
                if (best_abs_tick == -1 || abs_tick > best_abs_tick) best_abs_tick = abs_tick;
            } else {
                if (best_abs_tick == -1 || abs_tick < best_abs_tick) best_abs_tick = abs_tick;
            }
        }
    }

    int64_t bestAbsTick() const { return best_abs_tick; }
    const Window& getWindow() const { return window; }

    bool isInBounds(int64_t abs_tick) const {
        int64_t rel_idx = abs_tick - window.base_tick;
        return rel_idx >= 0 && rel_idx < static_cast<int64_t>(window.size);
    }

    bool shouldRecenter(int64_t abs_tick) const {
        if (window.size == 0) return true;
        const int64_t rel = abs_tick - window.base_tick;
        const int64_t size = static_cast<int64_t>(window.size);
        const int64_t margin = std::max<int64_t>(
            1,
            static_cast<int64_t>(size * HYSTERESIS_MARGIN_RATIO)
        );
        if (rel < 0 || rel >= size) return true;
        if (window.base_tick > 0 && rel < margin) return true;
        if (rel >= size - margin) return true;
        return false;
    }

    // DECLARATION only – definition is after the class
    void recalcBest();

    // For debugging
    #ifdef DEBUG_RECENTER
    std::vector<int64_t> activeTicks() const {
        std::vector<int64_t> ticks;
        for (size_t word_idx = 0; word_idx < window.bitmap.size(); ++word_idx) {
            uint64_t word = window.bitmap[word_idx];
            while (word) {
                int bit = __builtin_ctzll(word);
                word &= word - 1;
                int64_t rel_idx = (static_cast<int64_t>(word_idx) << 6) + bit;
                ticks.push_back(window.base_tick + rel_idx);
            }
        }
        std::sort(ticks.begin(), ticks.end());
        return ticks;
    }

    bool verifyConsistency() const {
        bool ok = true;
        for (size_t idx = 0; idx < window.levels.size(); ++idx) {
            bool active = (window.levels[idx].quantity != 0);
            bool bit_set = (window.bitmap[idx >> 6] & (1ULL << (idx & 63))) != 0;
            if (active != bit_set) {
                std::cerr << "[SideBook] Inconsistency: idx=" << idx
                          << " active=" << active << " bit_set=" << bit_set
                          << " qty=" << window.levels[idx].quantity << std::endl;
                ok = false;
            }
        }
        return ok;
    }

    void verifyRecenter(const std::vector<int64_t>& before, const std::vector<int64_t>& after) {
        static size_t mismatch_count = 0;
        if (before.size() != after.size()) {
            std::cerr << "[RECENTER BUG] Active level count mismatch: before=" << before.size()
                      << ", after=" << after.size() << std::endl;
            size_t i = 0, j = 0;
            while (i < before.size() && j < after.size()) {
                if (before[i] != after[j]) {
                    std::cerr << "  diff: before=" << before[i] << " after=" << after[j] << std::endl;
                    if (before[i] < after[j]) ++i; else ++j;
                } else {
                    ++i; ++j;
                }
                if (++mismatch_count > 50) break;
            }
            if (mismatch_count >= 50) {
                std::cerr << "[RECENTER] Too many mismatches, suppressing further logs." << std::endl;
            }
        }
    }
    #endif

    void recenter(int64_t abs_tick);

    void setBestTick(int64_t tick) { best_abs_tick = tick; }

private:
    Window window;
    int64_t best_abs_tick = -1;

    // Declare the helper used by recalcBest
    int64_t findBestRelative() const;
};

// ----------------------------------------------------------------------------
// Implementation of template member functions (must be in header)
// ----------------------------------------------------------------------------

template<Side S>
int64_t SideBook<S>::findBestRelative() const {
    if constexpr (S == Side::Bid) {
        for (int w = static_cast<int>(window.bitmap.size()) - 1; w >= 0; --w) {
            uint64_t word = window.bitmap[static_cast<size_t>(w)];
            if (word != 0) {
                return (static_cast<int64_t>(w) << 6) + (63 - __builtin_clzll(word));
            }
        }
    } else {
        for (size_t w = 0; w < window.bitmap.size(); ++w) {
            uint64_t word = window.bitmap[w];
            if (word != 0) {
                return (static_cast<int64_t>(w) << 6) + __builtin_ctzll(word);
            }
        }
    }
    return -1;
}

template<Side S>
void SideBook<S>::recalcBest() {
    int64_t rel_idx = findBestRelative();
    if (rel_idx != -1) {
        best_abs_tick = window.base_tick + rel_idx;
    } else {
        best_abs_tick = -1;
    }
}

template<Side S>
void SideBook<S>::recenter(int64_t abs_tick) {
    #ifdef DEBUG_RECENTER
    auto before = activeTicks();
    #endif

    int64_t min_active = window.base_tick + static_cast<int64_t>(window.size);
    int64_t max_active = window.base_tick;
    bool has_active = false;

    for (size_t word_idx = 0; word_idx < window.bitmap.size(); ++word_idx) {
        uint64_t word = window.bitmap[word_idx];
        while (word) {
            int bit = __builtin_ctzll(word);
            word &= word - 1;
            int64_t rel_idx = (static_cast<int64_t>(word_idx) << 6) + bit;
            if (rel_idx >= static_cast<int64_t>(window.levels.size())) continue;
            int64_t abs_tick_active = window.base_tick + rel_idx;
            if (abs_tick_active < min_active) min_active = abs_tick_active;
            if (abs_tick_active > max_active) max_active = abs_tick_active;
            has_active = true;
        }
    }

    if (!has_active) {
        min_active = abs_tick;
        max_active = abs_tick;
    } else {
        if (abs_tick < min_active) min_active = abs_tick;
        if (abs_tick > max_active) max_active = abs_tick;
    }

    int64_t range = max_active - min_active;
    int64_t margin = static_cast<int64_t>(static_cast<double>(range) * 0.10) + 1000;
    if (margin < 1) margin = 1;

    int64_t new_min = min_active - margin;
    int64_t new_max = max_active + margin;
    int64_t new_size = new_max - new_min + 1;

    if (new_size < static_cast<int64_t>(window.size)) {
        int64_t mid = (min_active + max_active) / 2;
        new_min = mid - static_cast<int64_t>(window.size) / 2;
        new_max = new_min + static_cast<int64_t>(window.size) - 1;
        new_size = static_cast<int64_t>(window.size);
    }

    if (new_min < 0) {
        new_min = 0;
        new_max = new_min + new_size - 1;
    }

    Window new_win;
    new_win.base_tick = new_min;
    new_win.size = static_cast<size_t>(new_size);
    new_win.levels.resize(new_win.size);
    new_win.bitmap.resize((new_win.size >> 6) + 1, 0);

    std::cout << "[Recenter] old_base=" << window.base_tick
              << " old_size=" << window.size
              << " new_base=" << new_win.base_tick
              << " new_size=" << new_win.size
              << " active_range: " << min_active << " -> " << max_active
              << " margin=" << margin << std::endl;

    // Log first and last 5 active levels (for diagnostics)
    std::cout << "[recenter] Old active levels (first 5):";
    size_t count = 0;
    for (size_t word_idx = 0; word_idx < window.bitmap.size() && count < 5; ++word_idx) {
        uint64_t word = window.bitmap[word_idx];
        while (word && count < 5) {
            int bit = __builtin_ctzll(word);
            word &= word - 1;
            int64_t rel_idx = (static_cast<int64_t>(word_idx) << 6) + bit;
            std::cout << " (" << (window.base_tick + rel_idx) << ","
                      << window.levels[rel_idx].quantity << ")";
            ++count;
        }
    }
    std::cout << std::endl;

    std::cout << "[recenter] Old active levels (last 5):";
    size_t count_last = 0;
    for (int word_idx = static_cast<int>(window.bitmap.size()) - 1;
         word_idx >= 0 && count_last < 5; --word_idx) {
        uint64_t word = window.bitmap[static_cast<size_t>(word_idx)];
        while (word && count_last < 5) {
            int bit = 63 - __builtin_clzll(word);
            word &= ~(1ULL << bit);
            int64_t rel_idx = (static_cast<int64_t>(word_idx) << 6) + bit;
            if (rel_idx >= static_cast<int64_t>(window.levels.size())) continue;
            std::cout << " (" << (window.base_tick + rel_idx) << ","
                      << window.levels[rel_idx].quantity << ")";
            ++count_last;
        }
    }
    std::cout << std::endl;

    // Copy active levels
    for (size_t word_idx = 0; word_idx < window.bitmap.size(); ++word_idx) {
        uint64_t word = window.bitmap[word_idx];
        while (word) {
            int bit = __builtin_ctzll(word);
            word &= word - 1;
            int64_t old_rel_idx = (static_cast<int64_t>(word_idx) << 6) + bit;
            if (old_rel_idx >= static_cast<int64_t>(window.levels.size())) continue;

            int64_t abs_tick_active = window.base_tick + old_rel_idx;
            int64_t new_rel_idx = abs_tick_active - new_win.base_tick;

            if (new_rel_idx >= 0 && new_rel_idx < static_cast<int64_t>(new_win.size)) {
                new_win.levels[static_cast<size_t>(new_rel_idx)] =
                    window.levels[static_cast<size_t>(old_rel_idx)];
                new_win.bitmap[static_cast<size_t>(new_rel_idx >> 6)] |=
                    (1ULL << (new_rel_idx & 63));
            } else {
                std::cerr << "WARNING: Active level lost during recenter! "
                          << "abs_tick=" << abs_tick_active
                          << " new_min=" << new_min
                          << " new_max=" << new_max << std::endl;
            }
        }
    }

    window = std::move(new_win);

    #ifdef DEBUG_RECENTER
    auto after = activeTicks();
    verifyRecenter(before, after);
    #endif

    recalcBest();
}

using BidSide = SideBook<Side::Bid>;
using AskSide = SideBook<Side::Ask>;