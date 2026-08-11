#include "engine/diff_history_buffer.hpp"

#include <cassert>
#include <iostream>



DiffHistoryBuffer::DiffHistoryBuffer(size_t capacity)
    : buffer_(capacity)
    , capacity_(capacity) {
    assert(capacity > 0 && "DiffHistoryBuffer capacity must be > 0");
}


/**
 * head_
   ↓
    oldest Delta in physical storage

    size_
   ↓
    number of valid Deltas

    capacity_
   ↓
    maximum number of Deltas

    oldest_u_
   ↓
    u of oldest retained Delta
 */
void DiffHistoryBuffer::push(Delta&& delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    const uint64_t seq = delta.u;

    if (size_ < capacity_) {
        size_t idx = (head_ + size_) % capacity_;
        buffer_[idx] = std::move(delta);
        ++size_;
        if (size_ == 1) {
            oldest_u_ = seq;
        }
        newest_u_ = seq;
    } else {
        // Overwrite the oldest entry.
        buffer_[head_] = std::move(delta);
        head_ = (head_ + 1) % capacity_;
        oldest_u_ = buffer_[head_].u;
        newest_u_ = seq;
    }
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
        std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                  << ") failed: target sequence not yet available. "
                  << "newest_u=" << newest_u_ << std::endl;
        return false;
    }

    uint64_t current = from_seq;
    bool found_first = false;

    // Start scanning from the oldest retained entry.
    size_t idx = head_;
    for (size_t i = 0; i < size_; ++i) {
        const Delta& d = buffer_[idx];
        idx = (idx + 1) % capacity_;
        if (!found_first) {
            // Check if this delta covers the next needed sequence (current+1).
            if (d.U <= current + 1 && d.u >= current + 1) {
                // If this delta overshoots to_seq, we cannot use it.
                if (d.u > to_seq) {
                    std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                              << ") failed: first delta overshoots (u=" << d.u
                              << " > " << to_seq << ")" << std::endl;
                    out.clear();
                    return false;
                }
                // Include this delta.
                out.push_back(d);
                current = d.u;
                found_first = true;
                // If we've already reached or passed to_seq, stop.
                if (current >= to_seq) break;
            } else if (d.U > current + 1) {
                // Gap before the first event: we cannot start.
                std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                          << ") failed: gap before first event. Expected "
                          << (current + 1) << " but got U=" << d.U << std::endl;
                out.clear();
                return false;
            }
            // Otherwise, d.u <= current, so it's too old; continue scanning.
        } else {
            // Already found the first event. Now look for chaining events.
            // Skip events that are already fully covered.
            if (d.u <= current) {
                // Nothing to add.
                continue;
            }
            // Check for a gap.
            if (d.U > current + 1) {
                std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                          << ") failed: gap. Current=" << current
                          << ", next U=" << d.U << std::endl;
                out.clear();
                return false;
            }
            // If this delta overshoots to_seq, we cannot use it.
            if (d.u > to_seq) {
                std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                          << ") failed: delta overshoots (u=" << d.u
                          << " > " << to_seq << ")" << std::endl;
                out.clear();
                return false;
            }
            // The delta chains correctly; include it.
            out.push_back(d);
            current = d.u;
            if (current >= to_seq) break;
        }

    }

    // After the loop, verify that we found a first event.
    if (!found_first) {
        std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                  << ") failed: no event covers sequence " << (from_seq + 1)
                  << std::endl;
        return false;
    }

    // Verify that we reached exactly to_seq.
    if (current != to_seq) {
        // This should not happen if overshoots are rejected and gaps are caught.
        // But keep it as a safety net.
        std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                  << ") failed: final sequence " << current
                  << " != " << to_seq << std::endl;
        out.clear();
        return false;
    }

    // Success.
    std::cout << "[History] getRange(" << from_seq << ", " << to_seq
              << ") success: " << out.size() << " events, final seq "
              << current << std::endl;
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