#include "engine/diff_history_buffer.hpp"

#include <cassert>
#include <iostream>

DiffHistoryBuffer::DiffHistoryBuffer(size_t capacity)
    : buffer_(capacity)
    , capacity_(capacity) {
    assert(capacity > 0 && "DiffHistoryBuffer capacity must be > 0");
}

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



bool DiffHistoryBuffer::getRange(
    uint64_t from_seq,
    uint64_t to_seq,
    std::vector<Delta>& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    out.clear();

    if (size_ == 0)
        return false;

    // ------------------------------------------------------------
    // Verify that the requested range is still retained.
    // We need an event covering from_seq + 1, therefore
    // from_seq + 1 must not be older than our oldest sequence.
    // ------------------------------------------------------------

    std::cout << "[History] buffer range in getRange() : " << oldest_u_ << " - " << newest_u_ << std::endl;
    std::cout << "[History] getRange(" << from_seq << ", " << to_seq << ") returned "
          << out.size() << " deltas:";
    for (const auto& d : out) {
        std::cout << " (U=" << d.U << ", u=" << d.u << ")";
    }
    std::cout << std::endl;

    if (oldest_u_ > 0 && from_seq + 1 < oldest_u_) {
        std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                  << ") failed: start sequence not retained. "
                  << "oldest_u=" << oldest_u_ << std::endl;
        return false;
    }

    if (to_seq > newest_u_) {
        std::cout << "[History] getRange(" << from_seq << ", " << to_seq
                  << ") failed: target sequence not yet available. "
                  << "newest_u=" << newest_u_ << std::endl;
        return false;
    }

    uint64_t current = from_seq;
    bool found_first = false;

    size_t idx = head_;

    // ------------------------------------------------------------
    // Buffer is chronological (oldest -> newest).
    // Build one continuous replay chain.
    // ------------------------------------------------------------
    for (size_t i = 0; i < size_; ++i) {

        const Delta& d = buffer_[idx];

        idx = (idx + 1) % capacity_;

        // Haven't found the starting event yet.
        if (!found_first) {

            // Too old.
            if (d.u <= current)
                continue;

            // First valid event.
            if (d.U <= current + 1 && d.u >= current + 1) {

                out.push_back(d);
                current = d.u;
                found_first = true;

                if (current >= to_seq)
                    break;
            }

            // Because entries are chronological, once U exceeds
            // current+1 we have permanently lost continuity.
            else if (d.U > current + 1) {

                std::cout
                    << "[History] Missing first replay event. "
                    << "Expected sequence " << current + 1
                    << " but next event begins at U=" << d.U
                    << std::endl;

                out.clear();
                return false;
            }

            continue;
        }

        // Ignore already-covered events.
        if (d.u <= current)
            continue;

        // Gap in replay chain.
        if (d.U > current + 1) {

            std::cout
                << "[History] Replay gap. "
                << "Current=" << current
                << " next U=" << d.U
                << std::endl;

            out.clear();
            return false;
        }

        // Chains correctly.
        out.push_back(d);
        current = d.u;

        if (current >= to_seq)
            break;
    }

    std::cout << "[Replay] Replayed " << out.size() << " events, U="
          << (out.empty() ? 0 : out.front().U)
          << " .. " << (out.empty() ? 0 : out.back().u)
          << std::endl;

    if (!found_first) {

        std::cout
            << "[History] No event covers sequence "
            << from_seq + 1
            << std::endl;

        return false;
    }

    if (current < to_seq) {

        std::cout
            << "[History] Replay incomplete. "
            << "Reached " << current
            << " expected " << to_seq
            << std::endl;

        out.clear();
        return false;
    }

    std::cout
        << "[History] getRange("
        << from_seq << ", "
        << to_seq << ") success. "
        << out.size() << " events, final sequence "
        << current
        << std::endl;

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