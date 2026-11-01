#pragma once

#include "engine/delta.hpp"

#include <mutex>
#include <vector>
#include <cstdint>

/**
 * DiffHistoryBuffer – Thread‑safe ring buffer for parsed Deltas.
 *
 * Storage is two rings allocated once in the constructor:
 *   - levels_  : every bid/ask LevelUpdate of every retained Delta, back to back
 *   - entries_ : one {U, u, level offset, counts} record per retained Delta
 *
 * push() only copies into these rings, so the engine thread never allocates
 * on the hot path. An entry is evicted when either ring runs out of room;
 * getRange() then reports the range as unavailable and the Validator skips.
 */
class DiffHistoryBuffer {
public:
    /**
     * @param capacity        max number of Deltas retained
     * @param level_capacity  max number of LevelUpdates retained across all
     *                        Deltas (0 = capacity * 64)
     */
    explicit DiffHistoryBuffer(size_t capacity = 50000, size_t level_capacity = 0);

    // Copies the Delta in; the caller keeps its vectors (and their capacity).
    void push(const Delta& delta);

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
    size_t levelCapacity() const;

private:
    struct Entry {
        uint64_t U = 0;
        uint64_t u = 0;
        uint64_t level_start = 0;  // absolute index into the level stream
        uint32_t n_bids = 0;
        uint32_t n_asks = 0;
    };

    void evictOldestLocked();
    void copyLevelsIn(const std::vector<LevelUpdate>& src);
    void copyLevelsOut(uint64_t start, uint32_t count, std::vector<LevelUpdate>& dst) const;

    mutable std::mutex mutex_;

    std::vector<Entry> entries_;
    size_t head_ = 0;      // index of the oldest entry
    size_t size_ = 0;      // number of retained entries
    size_t capacity_;

    std::vector<LevelUpdate> levels_;
    size_t level_capacity_;
    uint64_t level_head_ = 0;  // absolute index of the next level to write
    uint64_t level_tail_ = 0;  // absolute index of the oldest retained level

    uint64_t oldest_u_ = 0;
    uint64_t newest_u_ = 0;
};
