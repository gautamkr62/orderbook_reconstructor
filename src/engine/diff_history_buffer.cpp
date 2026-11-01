#include "engine/diff_history_buffer.hpp"

#include <algorithm>
#include <cassert>

DiffHistoryBuffer::DiffHistoryBuffer(size_t capacity, size_t level_capacity)
    : entries_(capacity)
    , capacity_(capacity)
    , levels_(level_capacity ? level_capacity : capacity * 64)
    , level_capacity_(levels_.size()) {
    assert(capacity > 0 && "DiffHistoryBuffer capacity must be > 0");
}

/**
 * head_         index of the oldest Entry in entries_
 * size_         number of retained Entries
 * level_tail_   absolute index of the first level of the oldest Entry
 * level_head_   absolute index where the next level is written
 *               (absolute indices only grow; position in levels_ is index % level_capacity_)
 * oldest_u_     u of the oldest retained Delta
 */
void DiffHistoryBuffer::evictOldestLocked() {
    const Entry& e = entries_[head_];
    level_tail_ = e.level_start + e.n_bids + e.n_asks;
    head_ = (head_ + 1) % capacity_;
    --size_;
}

void DiffHistoryBuffer::copyLevelsIn(const std::vector<LevelUpdate>& src) {
    size_t pos = static_cast<size_t>(level_head_ % level_capacity_);
    size_t done = 0;
    while (done < src.size()) {
        const size_t n = std::min(src.size() - done, level_capacity_ - pos);
        std::copy_n(src.data() + done, n, levels_.data() + pos);
        done += n;
        pos = 0;  // wrapped
    }
    level_head_ += src.size();
}

void DiffHistoryBuffer::copyLevelsOut(uint64_t start, uint32_t count,
                                      std::vector<LevelUpdate>& dst) const {
    dst.resize(count);
    size_t pos = static_cast<size_t>(start % level_capacity_);
    size_t done = 0;
    while (done < count) {
        const size_t n = std::min<size_t>(count - done, level_capacity_ - pos);
        std::copy_n(levels_.data() + pos, n, dst.data() + done);
        done += n;
        pos = 0;
    }
}

void DiffHistoryBuffer::push(const Delta& delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t n_levels = delta.bids.size() + delta.asks.size();

    if (n_levels > level_capacity_) {
        // Cannot be stored at all: drop everything so getRange() can never
        // return a chain with this Delta silently missing.
        head_ = size_ = 0;
        level_head_ = level_tail_ = 0;
        oldest_u_ = newest_u_ = delta.u;
        return;
    }

    // Make room in both rings.
    while (size_ > 0 &&
           (size_ == capacity_ || level_head_ + n_levels - level_tail_ > level_capacity_)) {
        evictOldestLocked();
    }
    if (size_ == 0) {
        level_tail_ = level_head_;
    }

    Entry& e = entries_[(head_ + size_) % capacity_];
    e.U = delta.U;
    e.u = delta.u;
    e.level_start = level_head_;
    e.n_bids = static_cast<uint32_t>(delta.bids.size());
    e.n_asks = static_cast<uint32_t>(delta.asks.size());
    copyLevelsIn(delta.bids);
    copyLevelsIn(delta.asks);
    ++size_;

    oldest_u_ = entries_[head_].u;
    newest_u_ = delta.u;
}

bool DiffHistoryBuffer::getRange(uint64_t from_seq,
                                 uint64_t to_seq,
                                 std::vector<Delta>& out) const {
    // Lock the buffer for the entire operation.
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear the output vector (the caller expects this).
    out.clear();

    // Edge cases.
    if (size_ == 0) {
        return false;
    }
    if (from_seq >= to_seq) {
        // No range to replay → vacuously true.
        return true;
    }
    if (to_seq > newest_u_) {
        // The target sequence is beyond the latest data we have.
        return false;
    }

    auto emit = [&](const Entry& e) {
        Delta& d = out.emplace_back();
        d.U = e.U;
        d.u = e.u;
        copyLevelsOut(e.level_start, e.n_bids, d.bids);
        copyLevelsOut(e.level_start + e.n_bids, e.n_asks, d.asks);
    };

    uint64_t current = from_seq;
    bool found_first = false;

    // Start scanning from the oldest retained entry.
    size_t idx = head_;
    for (size_t i = 0; i < size_; ++i) {
        const Entry& d = entries_[idx];
        idx = (idx + 1) % capacity_;
        if (!found_first) {
            // Check if this delta covers the next needed sequence (current+1).
            if (d.U <= current + 1 && d.u >= current + 1) {
                // If this delta overshoots to_seq, we cannot use it.
                if (d.u > to_seq) {
                    out.clear();
                    return false;
                }
                emit(d);
                current = d.u;
                found_first = true;
                // If we've already reached or passed to_seq, stop.
                if (current >= to_seq) break;
            } else if (d.U > current + 1) {
                // Gap before the first event: we cannot start.
                out.clear();
                return false;
            }
            // Otherwise, d.u <= current, so it's too old; continue scanning.
        } else {
            // Already found the first event. Now look for chaining events.
            // Skip events that are already fully covered.
            if (d.u <= current) {
                continue;
            }
            // Check for a gap.
            if (d.U > current + 1) {
                out.clear();
                return false;
            }
            // If this delta overshoots to_seq, we cannot use it.
            if (d.u > to_seq) {
                out.clear();
                return false;
            }
            // The delta chains correctly; include it.
            emit(d);
            current = d.u;
            if (current >= to_seq) break;
        }
    }

    // After the loop, verify that we found a first event.
    if (!found_first) {
        return false;
    }

    // Verify that we reached exactly to_seq (safety net; overshoots and gaps
    // are already rejected above).
    if (current != to_seq) {
        out.clear();
        return false;
    }

    return true;
}

uint64_t DiffHistoryBuffer::oldestRetainedU() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return oldest_u_;
}

uint64_t DiffHistoryBuffer::newestRetainedU() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return newest_u_;
}

size_t DiffHistoryBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

size_t DiffHistoryBuffer::capacity() const {
    return capacity_;
}

size_t DiffHistoryBuffer::levelCapacity() const {
    return level_capacity_;
}
