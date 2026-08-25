#pragma once

#include "engine/delta.hpp"

#include <mutex>
#include <vector>
#include <cstdint>

/**
 * DiffHistoryBuffer – Thread‑safe ring buffer for parsed Deltas.
 */
class DiffHistoryBuffer {
public:
    explicit DiffHistoryBuffer(size_t capacity = 50000);

    void push(Delta&& delta);
    void push(Delta& delta);


    /**
     * Retrieves all Deltas in (from_seq, to_seq] that form a valid, gapless chain.
     * Returns false if:
     *   - The range is partially overwritten (wraparound).
     *   - There is a gap in the sequence numbers within the range.
     */
    bool getRange(uint64_t from_seq, uint64_t to_seq, std::vector<Delta>& out) const;

    uint64_t oldestRetainedU() const;
    uint64_t newestRetainedU() const;
    size_t size() const;
    size_t capacity() const;

private:
    mutable std::mutex mutex_;

    std::vector<Delta> buffer_;
    size_t head_ = 0;
    size_t size_ = 0;
    size_t capacity_;

    uint64_t oldest_u_ = 0;
    uint64_t newest_u_ = 0;
};