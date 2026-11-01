#pragma once

#include "price_level.hpp"
#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <iostream>


// ---------------------------------------------------------------------------
// Logging — single macro so it is trivial to redirect later
// ---------------------------------------------------------------------------
#define SB_LOG(fmt, ...) \
    std::fprintf(stderr, "[SideBook] " fmt "\n", ##__VA_ARGS__)

enum class Side { Bid, Ask };

template <Side S>
class SideBook {
public:
    struct Window {
        int64_t base_tick = 0;
        size_t  size      = 0;
        std::vector<PriceLevel> levels; // flat array; index == relative tick
        std::vector<uint64_t>   bitmap; // one bit per level slot
        std::vector<uint64_t> summary;
    };

    // Hysteresis: keep 10 % margin around the active range on each side,
    // plus a fixed 1 000-tick floor so tiny books still get breathing room.
    static constexpr double HYSTERESIS_MARGIN_RATIO = 0.10;

    // Uninitialized-use guard: all members have safe defaults.
    SideBook() = default;

    void init(int64_t mid_tick, size_t window_size);
    void updateLevel(int64_t abs_tick, uint64_t quantity);

    // Fast bulk-load for snapshot apply.
    // Sizes the window exactly from the data, writes all levels directly,
    // then recalcs bounds and best once. Zero recenters, one allocation.
    void applySnapshot(const std::pair<int64_t, uint64_t>* data, size_t count);

    int64_t       bestAbsTick() const { return best_abs_tick; }
    const Window& getWindow()   const { return window; }

    bool isInBounds    (int64_t abs_tick) const;
    bool shouldRecenter(int64_t abs_tick) const;

    void recalcBest();
    void recalcBestFrom(int64_t abs_tick); 
    void recalcBounds();
    void recalcBoundsFrom(int64_t abs_tick); 
    int64_t findBestRelativeFromHint(int64_t rel_hint) const; 

    std::vector<int64_t> activeTicks()        const;
    bool                 verifyConsistency()  const;

    // recenter is public so callers can force a manual rebalance if needed.
    void recenter(int64_t abs_tick, bool include_abs_tick = true);
    void setBestTick(int64_t tick) { best_abs_tick = tick; }

private:
    Window  window;
    int64_t best_abs_tick = -1;
    int64_t min_abs_tick  = -1;   // FIX: explicit initialiser (was uninitialized)
    int64_t max_abs_tick  = -1;   // FIX: explicit initialiser (was uninitialized)

    int64_t findBestRelative() const;

    // FIX: single canonical helper for bitmap word count.
    // (size + 63) / 64  is the correct ceiling division.
    // The old init() used (size >> 6) + 1, which allocates one extra word for
    // every size that is an exact multiple of 64, making init and recenter
    // disagree — leading to out-of-bounds bitmap writes.
    static size_t bitmapWords(size_t sz) { return (sz + 63) / 64; }
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

template<Side S>
void SideBook<S>::init(int64_t mid_tick, size_t window_size) {
    if (window_size == 0) {
        // SB_LOG("init called with window_size=0; using 2000");
        window_size = 2000;
    }

    window.size      = window_size;
    window.base_tick = mid_tick - static_cast<int64_t>(window_size / 2);
    if (window.base_tick < 0) window.base_tick = 0;

    window.levels.assign(window.size, PriceLevel{});
    window.bitmap.assign(bitmapWords(window.size), 0ULL);  // FIX: use bitmapWords()
    window.summary.assign((window.bitmap.size() >> 6) + 1, 0);


    best_abs_tick = -1;
    min_abs_tick  = -1;
    max_abs_tick  = -1;

    // SB_LOG("init mid=%lld size=%zu base=%lld",
    //        (long long)mid_tick,
    //        window.size,
    //        (long long)window.base_tick);
}

// ---------------------------------------------------------------------------

template<Side S>
bool SideBook<S>::isInBounds(int64_t abs_tick) const {
    const int64_t rel = abs_tick - window.base_tick;
    return rel >= 0 && rel < static_cast<int64_t>(window.size);
}

// ---------------------------------------------------------------------------

template<Side S>
bool SideBook<S>::shouldRecenter(int64_t abs_tick) const {
    // std::cout<<"ShouldRecenter is called\n";
    // std::cout<<"window.size() -> "<<window.size<<"\n"; 
    if (window.size == 0)
        return true;
    // Compute what the active range would be after this tick is included.
    int64_t lo = (min_abs_tick == -1) ? abs_tick : std::min(min_abs_tick, abs_tick);
    int64_t hi = (max_abs_tick == -1) ? abs_tick : std::max(max_abs_tick, abs_tick);

    const int64_t active_range = hi - lo;
    const int64_t margin =
        static_cast<int64_t>(static_cast<double>(active_range) * HYSTERESIS_MARGIN_RATIO)
        + 1000;
    const int64_t required_size = active_range + 2 * margin + 1;

    return required_size > static_cast<int64_t>(window.size);
}

// ---------------------------------------------------------------------------

template<Side S>
int64_t SideBook<S>::findBestRelative() const {
    if constexpr (S == Side::Bid) {
        for (int w = static_cast<int>(window.bitmap.size()) - 1; w >= 0; --w) {
            const uint64_t word = window.bitmap[static_cast<size_t>(w)];
            if (word != 0)
                return (static_cast<int64_t>(w) << 6) + (63 - __builtin_clzll(word));
        }
    } else {
        for (size_t w = 0; w < window.bitmap.size(); ++w) {
            const uint64_t word = window.bitmap[w];
            if (word != 0)
                return (static_cast<int64_t>(w) << 6) + __builtin_ctzll(word);
        }
    }
    return -1;
}

template<Side S>
int64_t SideBook<S>::findBestRelativeFromHint(int64_t rel_hint) const {
    const int64_t word_idx = rel_hint >> 6;
    const int bit = static_cast<int>(rel_hint & 63);

    if constexpr (S == Side::Bid) {

        // New best must be BELOW the deleted level.
        for (int64_t w = word_idx; w >= 0; --w) {
            uint64_t word = window.bitmap[static_cast<size_t>(w)];

            if (w == word_idx) {
                word &= (bit == 0)
                    ? 0ULL
                    : ((1ULL << bit) - 1ULL);
            }

            if (word != 0) {
                return (w << 6) +
                       (63 - __builtin_clzll(word));
            }
        }

    } else {

        // New best must be ABOVE the deleted level.
        for (int64_t w = word_idx;
             w < static_cast<int64_t>(window.bitmap.size());
             ++w) {

            uint64_t word = window.bitmap[static_cast<size_t>(w)];

            if (w == word_idx) {
                if (bit == 63)
                    word = 0;
                else
                    word &= ~((1ULL << (bit + 1)) - 1ULL);
            }

            if (word != 0) {
                return (w << 6) +
                       __builtin_ctzll(word);
            }
        }
    }

    return -1;
}

template<Side S>
void SideBook<S>::recalcBestFrom(int64_t abs_tick) {
    const int64_t rel_hint =
        abs_tick - window.base_tick;

    const int64_t rel_idx =
        findBestRelativeFromHint(rel_hint);

    best_abs_tick =
        (rel_idx != -1)
            ? window.base_tick + rel_idx
            : -1;
}

// ---------------------------------------------------------------------------

template<Side S>
void SideBook<S>::recalcBest() {

    // std::cout<<"recalcBest is called\n"; 

    const int64_t rel = findBestRelative();
    best_abs_tick = (rel != -1) ? (window.base_tick + rel) : -1;
}

// ---------------------------------------------------------------------------

template<Side S>
void SideBook<S>::updateLevel(int64_t abs_tick, uint64_t quantity) {
    // Deletion for a tick that was never in the window: nothing to do.
    if (quantity == 0 && !isInBounds(abs_tick)) {
        // SB_LOG("updateLevel: delete tick=%lld ignored (out of window)",
        //        (long long)abs_tick);
        return;
    }

    // Expand / recenter if the tick falls outside or the window is too small.
    if (!isInBounds(abs_tick) || shouldRecenter(abs_tick)) {
        // recenter(abs_tick, quantity > 0);   // <-- you can re-enable if needed

        // After recenter the tick MUST be in bounds for insertions.
        // For deletions with no active levels, recenter is a no-op and the
        // tick is legitimately absent — nothing to erase.
        if (!isInBounds(abs_tick)) {
            SB_LOG("updateLevel: tick=%lld still out of bounds after recenter "
                   "(quantity=%llu); dropping",
                   (long long)abs_tick, (unsigned long long)quantity);
            return;
        }
    }

    const int64_t rel_idx = abs_tick - window.base_tick;
    if (rel_idx < 0 || rel_idx >= static_cast<int64_t>(window.size)) {
        // SB_LOG("updateLevel: unexpected rel_idx=%lld for tick=%lld; BUG",
        //        (long long)rel_idx, (long long)abs_tick);
        return;
    }

    PriceLevel& level    = window.levels[static_cast<size_t>(rel_idx)];
    const bool was_active = (level.quantity != 0);
    level.quantity        = quantity;

    const uint64_t mask = 1ULL << (static_cast<size_t>(rel_idx) & 63u);
    const size_t   word = static_cast<size_t>(rel_idx) >> 6u;

    if (quantity == 0) {
        // ---- Deletion -------------------------------------------------------
        window.bitmap[word] &= ~mask;

        // Update summary bitmap
        if (window.bitmap[word] == 0) {
            window.summary[word >> 6] &= ~(1ULL << (word & 63));
        }

        if (was_active) {
            if (abs_tick == min_abs_tick || abs_tick == max_abs_tick) {
                recalcBoundsFrom(abs_tick);   // now uses summary
            }

            if (abs_tick == best_abs_tick)
                recalcBestFrom(abs_tick);
        }
    } else {
        // ---- Insertion / update ---------------------------------------------
        if (!was_active) {
            window.bitmap[word] |= mask;

            // Update summary bitmap
            window.summary[word >> 6] |= (1ULL << (word & 63));

            if (min_abs_tick == -1) {
                min_abs_tick = abs_tick;
                max_abs_tick = abs_tick;
            } else {
                if (abs_tick < min_abs_tick) min_abs_tick = abs_tick;
                if (abs_tick > max_abs_tick) max_abs_tick = abs_tick;
            }
        }

        if constexpr (S == Side::Bid) {
            if (best_abs_tick == -1 || abs_tick > best_abs_tick)
                best_abs_tick = abs_tick;
        } else {
            if (best_abs_tick == -1 || abs_tick < best_abs_tick)
                best_abs_tick = abs_tick;
        }
    }
}


// ---------------------------------------------------------------------------

template<Side S>
void SideBook<S>::recenter(int64_t abs_tick, bool include_abs_tick) {

    std::cout<<"recenter is called\n"; 
    int64_t lo = min_abs_tick;
    int64_t hi = max_abs_tick;

    if (lo == -1 || hi == -1) {
        // No active levels yet.
        if (!include_abs_tick) {
            // Deletion with empty book — nothing to recenter.
            // SB_LOG("recenter: empty book, deletion tick=%lld; skipping",
            //        (long long)abs_tick);
            return;
        }
        lo = hi = abs_tick;
    } else if (include_abs_tick) {
        if (abs_tick < lo) lo = abs_tick;
        if (abs_tick > hi) hi = abs_tick;
    }

    const int64_t active_range = hi - lo;
    const int64_t margin =
        static_cast<int64_t>(static_cast<double>(active_range) * HYSTERESIS_MARGIN_RATIO)
        + 1000;
    const int64_t required_size = active_range + 2 * margin + 1;
    const int64_t doubled_size  = static_cast<int64_t>(window.size) * 2;
    int64_t       new_size      = std::max(doubled_size, required_size);

    if (new_size > 100'000'000LL) {
        // SB_LOG("recenter: new_size=%lld exceeds 100M limit; throwing",
        //        (long long)new_size);
        throw std::runtime_error("Recenter window size explosion");
    }

    // Center the new window on the midpoint of the active range.
    const int64_t active_mid = lo + (hi - lo) / 2;
    int64_t       new_base   = active_mid - new_size / 2;
    if (new_base < 0) new_base = 0;

    // FIX: after the clamp, verify abs_tick (if being included) still fits.
    // If the base was clamped upward and abs_tick is large, the tick could
    // fall beyond new_base + new_size.  Extend new_size to cover it.
    if (include_abs_tick) {
        const int64_t needed = abs_tick - new_base + 1;
        if (needed > new_size) {
            // SB_LOG("recenter: extending new_size %lld -> %lld to cover tick=%lld after base clamp",
            //        (long long)new_size, (long long)needed, (long long)abs_tick);
            new_size = needed;

            // Re-check the hard limit after the extension.
            if (new_size > 100'000'000LL) {
                // SB_LOG("recenter: extended new_size=%lld exceeds 100M limit; throwing",
                //        (long long)new_size);
                throw std::runtime_error("Recenter window size explosion");
            }
        }
    }

    // SB_LOG("recenter: old_base=%lld old_size=%zu -> new_base=%lld new_size=%lld "
    //        "(active=[%lld,%lld] include_tick=%d tick=%lld)",
    //        (long long)window.base_tick, window.size,
    //        (long long)new_base, (long long)new_size,
    //        (long long)lo, (long long)hi,
    //        (int)include_abs_tick, (long long)abs_tick);

    // ------------------------------------------------------------------
    // Build the new window in-place.
    // ------------------------------------------------------------------
    Window old_window = std::move(window);   // O(1): just moves the vectors

    window.base_tick = new_base;
    window.size      = static_cast<size_t>(new_size);
    window.levels.assign(window.size, PriceLevel{});                 // FIX: assign (not construct) so capacity is reused where possible
    window.bitmap.assign(bitmapWords(window.size), 0ULL);            // FIX: bitmapWords() used consistently
    window.summary.assign((window.bitmap.size() + 63) / 64, 0ULL);   // rebuilt below with the bitmap

    // Copy only the active levels from the old window using the bitmap.
    for (size_t w = 0; w < old_window.bitmap.size(); ++w) {
        uint64_t word = old_window.bitmap[w];
        while (word) {
            const int    bit     = __builtin_ctzll(word);
            const size_t old_idx = (w << 6u) + static_cast<size_t>(bit);
            word &= word - 1;  // clear lowest set bit

            if (old_idx >= old_window.levels.size())
                continue;  // stale padding bit — skip

            const int64_t tick    = old_window.base_tick + static_cast<int64_t>(old_idx);
            const int64_t new_idx = tick - window.base_tick;

            if (new_idx < 0 || new_idx >= static_cast<int64_t>(window.size))
                continue;  // tick fell outside new window (shouldn't happen for active data)

            const size_t ni = static_cast<size_t>(new_idx);
            window.levels[ni]          = old_window.levels[old_idx];
            window.bitmap[ni >> 6u]   |= 1ULL << (ni & 63u);
            window.summary[ni >> 12u] |= 1ULL << ((ni >> 6u) & 63u);
        }
    }

    // Recompute derived state from the freshly built bitmap.
    // Order: bounds first, then best (best doesn't depend on bounds,
    // but keeping bounds fresh is good hygiene for any future change).
    recalcBounds();
    recalcBest();
}

// ---------------------------------------------------------------------------
template<Side S>
void SideBook<S>::recalcBounds() {
    // This full-edge scan is now only called from applySnapshot / recenter.
    // Live path uses recalcBoundsFrom() below.
    min_abs_tick = -1;
    max_abs_tick = -1;

    for (size_t w = 0; w < window.bitmap.size(); ++w) {
        const uint64_t word = window.bitmap[w];
        if (word == 0) continue;
        const size_t idx = (w << 6u) + static_cast<size_t>(__builtin_ctzll(word));
        if (idx < window.levels.size())
            min_abs_tick = window.base_tick + static_cast<int64_t>(idx);
        break;
    }
    for (int w = static_cast<int>(window.bitmap.size()) - 1; w >= 0; --w) {
        const uint64_t word = window.bitmap[static_cast<size_t>(w)];
        if (word == 0) continue;
        const size_t idx = (static_cast<size_t>(w) << 6u)
                         + static_cast<size_t>(63 - __builtin_clzll(word));
        if (idx < window.levels.size())
            max_abs_tick = window.base_tick + static_cast<int64_t>(idx);
        break;
    }
}

// New: scan inward from the deleted endpoint only.
template<Side S>
void SideBook<S>::recalcBoundsFrom(int64_t abs_tick) {
    const int64_t rel = abs_tick - window.base_tick;
    if (rel < 0 || rel >= static_cast<int64_t>(window.size)) return;

    const bool was_min = (abs_tick == min_abs_tick);
    const bool was_max = (abs_tick == max_abs_tick);

    const size_t deleted_word = static_cast<size_t>(rel) >> 6u;
    const int    deleted_bit  = static_cast<int>(rel & 63);

    if (was_min) {
        min_abs_tick = -1;

        // --- Step 1: finish scanning the deleted word itself (cheap, same as before) ---
        {
            uint64_t word = window.bitmap[deleted_word];
            word &= (deleted_bit == 63) ? 0ULL : ~((1ULL << (deleted_bit + 1)) - 1ULL);
            if (word != 0) {
                const size_t idx = (deleted_word << 6u) + static_cast<size_t>(__builtin_ctzll(word));
                if (idx < window.size)
                    min_abs_tick = window.base_tick + static_cast<int64_t>(idx);
            }
        }

        // --- Step 2: if nothing left in that word, use the summary to jump forward ---
        if (min_abs_tick == -1) {
            size_t sw = (deleted_word + 1) >> 6u;      // summary word covering the next bitmap word
            const int start_sbit = static_cast<int>((deleted_word + 1) & 63);
            const size_t summary_words = window.summary.size();

            uint64_t sword = (sw < summary_words)
                ? (window.summary[sw] & (start_sbit == 0 ? ~0ULL : (~0ULL << start_sbit)))
                : 0ULL;

            while (sword == 0 && ++sw < summary_words)
                sword = window.summary[sw];

            if (sword != 0) {
                const size_t word_idx = (sw << 6u) + static_cast<size_t>(__builtin_ctzll(sword));
                if (word_idx < window.bitmap.size()) {
                    uint64_t word = window.bitmap[word_idx];
                    if (word != 0) {
                        const size_t idx = (word_idx << 6u) + static_cast<size_t>(__builtin_ctzll(word));
                        if (idx < window.size)
                            min_abs_tick = window.base_tick + static_cast<int64_t>(idx);
                    }
                }
            }
        }
    }

    if (was_max) {
        max_abs_tick = -1;

        // --- Step 1: finish scanning the deleted word itself ---
        {
            uint64_t word = window.bitmap[deleted_word];
            word &= (deleted_bit == 0) ? 0ULL : ((1ULL << deleted_bit) - 1ULL);
            if (word != 0) {
                const size_t idx = (deleted_word << 6u) + static_cast<size_t>(63 - __builtin_clzll(word));
                if (idx < window.size)
                    max_abs_tick = window.base_tick + static_cast<int64_t>(idx);
            }
        }

        // --- Step 2: use the summary to jump backward ---
        if (max_abs_tick == -1 && deleted_word > 0) {
            int64_t sw = static_cast<int64_t>(deleted_word - 1) >> 6;
            const int start_sbit = static_cast<int>((deleted_word - 1) & 63);

            uint64_t sword = window.summary[static_cast<size_t>(sw)]
                              & ((start_sbit == 63) ? ~0ULL : ((1ULL << (start_sbit + 1)) - 1ULL));

            while (sword == 0 && --sw >= 0)
                sword = window.summary[static_cast<size_t>(sw)];

            if (sword != 0) {
                const size_t word_idx = (static_cast<size_t>(sw) << 6u)
                                       + static_cast<size_t>(63 - __builtin_clzll(sword));
                if (word_idx < window.bitmap.size()) {
                    uint64_t word = window.bitmap[word_idx];
                    if (word != 0) {
                        const size_t idx = (word_idx << 6u) + static_cast<size_t>(63 - __builtin_clzll(word));
                        if (idx < window.size)
                            max_abs_tick = window.base_tick + static_cast<int64_t>(idx);
                    }
                }
            }
        }
    }
}
// ---------------------------------------------------------------------------

template<Side S>
std::vector<int64_t> SideBook<S>::activeTicks() const {
    std::vector<int64_t> ticks;
    for (size_t w = 0; w < window.bitmap.size(); ++w) {
        uint64_t word = window.bitmap[w];
        while (word) {
            const int    bit     = __builtin_ctzll(word);
            const int64_t rel   = (static_cast<int64_t>(w) << 6) + bit;
            word &= word - 1;
            ticks.push_back(window.base_tick + rel);
        }
    }
    std::sort(ticks.begin(), ticks.end());
    return ticks;
}

// ---------------------------------------------------------------------------

template<Side S>
void SideBook<S>::applySnapshot(const std::pair<int64_t, uint64_t>* data, size_t count) {
    // Helper: clear only active slots using summary to skip empty words.
    // This is O(active_words + active_levels), not O(window_words).
    auto clearActive = [this]() {
        for (size_t sw = 0; sw < window.summary.size(); ++sw) {
            uint64_t sword = window.summary[sw];
            while (sword) {
                int sbit = __builtin_ctzll(sword);
                sword &= sword - 1;
                size_t w = (sw << 6u) + static_cast<size_t>(sbit);
                if (w >= window.bitmap.size()) continue;
                uint64_t word = window.bitmap[w];
                while (word) {
                    int b = __builtin_ctzll(word);
                    word &= word - 1;
                    size_t idx = (w << 6u) + static_cast<size_t>(b);
                    if (idx < window.levels.size())
                        window.levels[idx].quantity = 0;
                }
                window.bitmap[w] = 0;
            }
            window.summary[sw] = 0;  // all words in this summary block are now zero
        }
    };

    if (count == 0) {
        clearActive();
        best_abs_tick = min_abs_tick = max_abs_tick = -1;
        return;
    }

    int64_t lo = data[0].first;
    int64_t hi = data[0].first;
    for (size_t i = 1; i < count; ++i) {
        const int64_t t = data[i].first;
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }

    const int64_t cur_end = window.base_tick + static_cast<int64_t>(window.size);
    const bool fits = (window.size > 0) && (lo >= window.base_tick) && (hi < cur_end);

    if (fits) {
        // Fast path: only clears active levels
        clearActive();
    } else {
        // Slow path: reallocate window and summary, both zeroed
        const int64_t active_range = hi - lo;
        const int64_t margin = static_cast<int64_t>(active_range * HYSTERESIS_MARGIN_RATIO) + 1000;
        int64_t new_size = std::max(active_range + 2 * margin + 1,
                                    static_cast<int64_t>(window.size));
        if (new_size <= 0 || new_size > 100'000'000LL)
            throw std::runtime_error("applySnapshot: window size out of range");

        int64_t new_base = lo - margin;
        if (new_base < 0) new_base = 0;
        const int64_t needed = hi - new_base + 1;
        if (needed > new_size) new_size = needed;
        if (new_size > 100'000'000LL)
            throw std::runtime_error("applySnapshot: window size out of range");

        window.base_tick = new_base;
        window.size = static_cast<size_t>(new_size);
        window.levels.assign(window.size, PriceLevel{});
        window.bitmap.assign(bitmapWords(window.size), 0ULL);
        window.summary.assign((window.bitmap.size() + 63) / 64, 0ULL);
    }

    best_abs_tick = min_abs_tick = max_abs_tick = -1;

    // Write levels and update summary incrementally
    for (size_t i = 0; i < count; ++i) {
        const int64_t tick = data[i].first;
        const uint64_t qty = data[i].second;
        if (qty == 0) continue;

        const size_t idx = static_cast<size_t>(tick - window.base_tick);
        window.levels[idx].quantity = qty;

        const size_t word = idx >> 6u;
        const uint64_t mask = 1ULL << (idx & 63u);
        const bool was_zero = (window.bitmap[word] == 0);
        window.bitmap[word] |= mask;
        if (was_zero) {
            window.summary[word >> 6] |= (1ULL << (word & 63));
        }
    }

    recalcBounds();
    recalcBest();
}
// ---------------------------------------------------------------------------

template<Side S>
bool SideBook<S>::verifyConsistency() const {
    for (size_t idx = 0; idx < window.levels.size(); ++idx) {
        const bool active  = (window.levels[idx].quantity != 0);
        const bool bit_set =
            (window.bitmap[idx >> 6u] & (1ULL << (idx & 63u))) != 0;
        if (active != bit_set) {
            // SB_LOG("verifyConsistency: mismatch at rel_idx=%zu (active=%d bit=%d)",
            //        idx, (int)active, (int)bit_set);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

using BidSide = SideBook<Side::Bid>;
using AskSide = SideBook<Side::Ask>;