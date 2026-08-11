#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

/*
end-to-end application processing latency per received delta

End-to-end in-process processing latency of a live WebSocket order-book update, from receipt by your handler until both the order book and history buffer have been updated.

*/

class MetricsCollector {
public:
    // -------------------------------------------------------------------------
    // Latency tracking
    // -------------------------------------------------------------------------

    void recordApplyLatency(std::chrono::microseconds latency) noexcept {
        const long long value = latency.count();

        const size_t idx =
            write_idx_.fetch_add(1, std::memory_order_relaxed)
            % LATENCY_BUF_SIZE;

        apply_latencies_[idx].store(value, std::memory_order_relaxed);

        const uint64_t old_count =
            latency_count_.fetch_add(1, std::memory_order_relaxed);

        // Once we exceed the ring capacity, the oldest samples are overwritten.
        if (old_count < LATENCY_BUF_SIZE) {
            // Nothing else required.
        }
    }

    void recordCriticalSectionLatency(std::chrono::microseconds latency) noexcept
    {
        const size_t idx =
            critical_section_write_idx_.fetch_add(
                1, std::memory_order_relaxed
            ) % LATENCY_BUF_SIZE;

        critical_section_latencies_[idx].store(
            latency.count(),
            std::memory_order_relaxed
        );

        critical_section_count_.fetch_add(
            1, std::memory_order_relaxed
        );
    }

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    void incValidationSuccess() noexcept {
        validation_success_.fetch_add(1, std::memory_order_relaxed);
    }

    void incValidationMismatch() noexcept {
        validation_mismatch_.fetch_add(1, std::memory_order_relaxed);
    }

    void incValidationSkipped() noexcept {
        validation_skipped_.fetch_add(1, std::memory_order_relaxed);
    }

    void incResyncTriggered() noexcept {
        resync_triggered_.fetch_add(1, std::memory_order_relaxed);
    }

    void incDeltaApplied() noexcept {
        delta_applied_.fetch_add(1, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Active book levels
    // -------------------------------------------------------------------------

    void setActiveBids(size_t n) noexcept {
        active_bids_.store(n, std::memory_order_relaxed);
    }

    void setActiveAsks(size_t n) noexcept {
        active_asks_.store(n, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Snapshot
    // -------------------------------------------------------------------------

    struct Snapshot {
        uint64_t delta_applied = 0;

        uint64_t validation_success = 0;
        uint64_t validation_mismatch = 0;
        uint64_t validation_skipped = 0;
        uint64_t resync_triggered = 0;

        size_t active_bids = 0;
        size_t active_asks = 0;

        double p50_latency_us = 0.0;
        double p99_latency_us = 0.0;

        // Number of latency observations currently represented
        // in the ring buffer, NOT lifetime number of observations.
        uint64_t latency_samples = 0;
    };

    Snapshot snapshot() const {
        Snapshot s;

        s.delta_applied =
            delta_applied_.load(std::memory_order_relaxed);

        s.validation_success =
            validation_success_.load(std::memory_order_relaxed);

        s.validation_mismatch =
            validation_mismatch_.load(std::memory_order_relaxed);

        s.validation_skipped =
            validation_skipped_.load(std::memory_order_relaxed);

        s.resync_triggered =
            resync_triggered_.load(std::memory_order_relaxed);

        s.active_bids =
            active_bids_.load(std::memory_order_relaxed);

        s.active_asks =
            active_asks_.load(std::memory_order_relaxed);

        // Number of valid samples currently available.
        const uint64_t total =
            latency_count_.load(std::memory_order_relaxed);

        const size_t n =
            static_cast<size_t>(
                std::min<uint64_t>(total, LATENCY_BUF_SIZE));

        if (n == 0)
            return s;

        std::vector<long long> samples;
        samples.reserve(n);

        /*
         * Read the ring.
         *
         * A concurrent writer can change a slot while we're reading it.
         * Since latency samples are independent observations, this gives us
         * an approximate point-in-time percentile snapshot without putting a
         * mutex on the hot path.
         */
        const size_t end =
            write_idx_.load(std::memory_order_relaxed);

        const size_t start =
            (end + LATENCY_BUF_SIZE - n) % LATENCY_BUF_SIZE;

        for (size_t i = 0; i < n; ++i) {
            const size_t idx =
                (start + i) % LATENCY_BUF_SIZE;

            const long long value =
                apply_latencies_[idx].load(std::memory_order_relaxed);

            if (value >= 0)
                samples.push_back(value);
        }

        if (samples.empty())
            return s;

        std::sort(samples.begin(), samples.end());

        s.p50_latency_us = percentile(samples, 0.50);
        s.p99_latency_us = percentile(samples, 0.99);

        s.latency_samples = samples.size();

        return s;
    }

    // -------------------------------------------------------------------------
    // Printing
    // -------------------------------------------------------------------------

    void printSnapshot() const {
        const Snapshot s = snapshot();

        std::cout
            << "\n========== Metrics Snapshot ==========\n"
            << "Deltas applied       : " << s.delta_applied << '\n'
            << "Validation success   : " << s.validation_success << '\n'
            << "Validation mismatches: " << s.validation_mismatch << '\n'
            << "Validation skipped   : " << s.validation_skipped << '\n'
            << "Resyncs triggered    : " << s.resync_triggered << '\n'
            << "Active bids/asks     : "
            << s.active_bids << " / " << s.active_asks << '\n'
            << "Latency samples      : " << s.latency_samples << '\n'
            << "Latency p50/p99      : "
            << s.p50_latency_us << "us / "
            << s.p99_latency_us << "us\n"
            << "=======================================\n"
            << std::endl;
    }

private:
    static constexpr size_t LATENCY_BUF_SIZE = 4096;

    // -------------------------------------------------------------------------
    // Percentile
    // -------------------------------------------------------------------------

    static double percentile(
        const std::vector<long long>& sorted,
        double p)
    {
        if (sorted.empty())
            return 0.0;

        if (sorted.size() == 1)
            return static_cast<double>(sorted[0]);

        // Linear interpolation between nearest ranks.
        const double position =
            p * static_cast<double>(sorted.size() - 1);

        const size_t lower =
            static_cast<size_t>(position);

        const size_t upper =
            lower + 1;

        if (upper >= sorted.size())
            return static_cast<double>(sorted.back());

        const double fraction =
            position - static_cast<double>(lower);

        return static_cast<double>(sorted[lower]) * (1.0 - fraction)
             + static_cast<double>(sorted[upper]) * fraction;
    }

private:
    // -------------------------------------------------------------------------
    // Latency ring
    // -------------------------------------------------------------------------

    std::array<std::atomic<long long>, LATENCY_BUF_SIZE>
        apply_latencies_{};

    std::atomic<size_t> write_idx_{0};

    // Lifetime number of recorded latency observations.
    std::atomic<uint64_t> latency_count_{0};

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    std::atomic<uint64_t> delta_applied_{0};

    std::atomic<uint64_t> validation_success_{0};
    std::atomic<uint64_t> validation_mismatch_{0};
    std::atomic<uint64_t> validation_skipped_{0};
    std::atomic<uint64_t> resync_triggered_{0};

    // -------------------------------------------------------------------------
    // Current book state
    // -------------------------------------------------------------------------

    std::atomic<size_t> active_bids_{0};
    std::atomic<size_t> active_asks_{0};



    std::array<std::atomic<long long>, LATENCY_BUF_SIZE>
    critical_section_latencies_{};

    std::atomic<size_t> critical_section_write_idx_{0};

    std::atomic<uint64_t> critical_section_count_{0};
};