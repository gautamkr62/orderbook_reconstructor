#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <mutex>

class MetricsCollector {
public:
    // -----------------------------------------------------------------
    // Latency tracking (call from processLiveDelta)
    // -----------------------------------------------------------------
    void recordApplyLatency(std::chrono::microseconds latency) {
        auto& buf = apply_latencies_[write_idx_.load(std::memory_order_relaxed)];
        buf = latency.count();
        write_idx_.store((write_idx_ + 1) % LATENCY_BUF_SIZE, std::memory_order_relaxed);
        apply_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------
    // Counters (call from Validator and Reconciler)
    // -----------------------------------------------------------------
    void incValidationSuccess() { validation_success_.fetch_add(1, std::memory_order_relaxed); }
    void incValidationMismatch() { validation_mismatch_.fetch_add(1, std::memory_order_relaxed); }
    void incValidationSkipped() { validation_skipped_.fetch_add(1, std::memory_order_relaxed); }
    void incResyncTriggered() { resync_triggered_.fetch_add(1, std::memory_order_relaxed); }
    void incDeltaApplied() { delta_applied_.fetch_add(1, std::memory_order_relaxed); }

    void setActiveBids(size_t n) { active_bids_ = n; }
    void setActiveAsks(size_t n) { active_asks_ = n; }

    // -----------------------------------------------------------------
    // Snapshot of current metrics
    // -----------------------------------------------------------------
    struct Snapshot {
        uint64_t delta_applied = 0;
        uint64_t validation_success = 0;
        uint64_t validation_mismatch = 0;
        uint64_t validation_skipped = 0;
        uint64_t resync_triggered = 0;
        size_t active_bids = 0;
        size_t active_asks = 0;
        double p50_latency_us = 0;
        double p99_latency_us = 0;
        uint64_t total_observations = 0;
    };

    Snapshot snapshot() const {
        Snapshot s;
        s.delta_applied = delta_applied_.load(std::memory_order_relaxed);
        s.validation_success = validation_success_.load(std::memory_order_relaxed);
        s.validation_mismatch = validation_mismatch_.load(std::memory_order_relaxed);
        s.validation_skipped = validation_skipped_.load(std::memory_order_relaxed);
        s.resync_triggered = resync_triggered_.load(std::memory_order_relaxed);
        s.active_bids = active_bids_;
        s.active_asks = active_asks_;

        // Compute percentiles from the ring buffer.
        std::vector<long long> samples;
        samples.reserve(LATENCY_BUF_SIZE);
        size_t idx = write_idx_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < LATENCY_BUF_SIZE; ++i) {
            long long val = apply_latencies_[(idx + i) % LATENCY_BUF_SIZE];
            if (val > 0) samples.push_back(val);
        }
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            size_t n = samples.size();
            s.p50_latency_us = samples[n / 2];
            s.p99_latency_us = samples[static_cast<size_t>(n * 0.99)];
            s.total_observations = n;
        }
        return s;
    }

    void printSnapshot() const {
        auto s = snapshot();
        std::cout << "\n========== Metrics Snapshot ==========\n";
        std::cout << "Deltas applied      : " << s.delta_applied << '\n';
        std::cout << "Validation success  : " << s.validation_success << '\n';
        std::cout << "Validation mismatches: " << s.validation_mismatch << '\n';
        std::cout << "Validation skipped  : " << s.validation_skipped << '\n';
        std::cout << "Resyncs triggered   : " << s.resync_triggered << '\n';
        std::cout << "Active bids/asks    : " << s.active_bids << " / " << s.active_asks << '\n';
        std::cout << "Latency (p50/p99)   : " << s.p50_latency_us << "us / " << s.p99_latency_us << "us\n";
        std::cout << "=======================================\n" << std::endl;
    }

private:
    static constexpr size_t LATENCY_BUF_SIZE = 4096;
    std::array<long long, LATENCY_BUF_SIZE> apply_latencies_{};
    std::atomic<size_t> write_idx_{0};

    std::atomic<uint64_t> apply_count_{0};
    std::atomic<uint64_t> delta_applied_{0};
    std::atomic<uint64_t> validation_success_{0};
    std::atomic<uint64_t> validation_mismatch_{0};
    std::atomic<uint64_t> validation_skipped_{0};
    std::atomic<uint64_t> resync_triggered_{0};

    std::atomic<size_t> active_bids_{0};
    std::atomic<size_t> active_asks_{0};
};