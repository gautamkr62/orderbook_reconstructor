#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

/*
Latencies are recorded in nanoseconds and reported in microseconds (2 dp).

end-to-end application processing latency per received delta

End-to-end in-process processing latency of a live WebSocket order-book update, from receipt by your handler until both the order book and history buffer have been updated.

*/

class MetricsCollector {
public:
    // -------------------------------------------------------------------------
    // Latency tracking
    // -------------------------------------------------------------------------
    explicit MetricsCollector(const std::string& name = "global") : name_(name) {}




    void recordParseLatency(std::chrono::nanoseconds latency) noexcept {
        const long long value = latency.count();
        const size_t idx =
            parse_write_idx_.fetch_add(1, std::memory_order_relaxed)
            % PARSE_LATENCY_BUF_SIZE;
        parse_latencies_[idx].store(value, std::memory_order_relaxed);
        parse_latency_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void recordE2ELatency(std::chrono::nanoseconds latency) noexcept {
        const long long value = latency.count();

        const size_t idx =
            e2e_write_idx_.fetch_add(1, std::memory_order_relaxed)
            % E2E_LATENCY_BUF_SIZE;

        e2e_latencies_[idx].store(value, std::memory_order_relaxed);

        e2e_latency_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void recordApplyLatency(std::chrono::nanoseconds latency) noexcept {
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

    void recordCriticalSectionLatency(std::chrono::nanoseconds latency) noexcept
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

        uint64_t e2e_latency_samples{0};
        double e2e_p50_us{0.0};
        double e2e_p99_us{0.0};

        uint64_t parse_latency_samples{0};
        double parse_p50_us{0.0};
        double parse_p99_us{0.0};


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

        // ---------------- APPLY LATENCY ----------------
        const uint64_t total =
            latency_count_.load(std::memory_order_relaxed);

        const size_t n =
            static_cast<size_t>(
                std::min<uint64_t>(total, LATENCY_BUF_SIZE));

        if (n > 0) {
            std::vector<long long> samples;
            samples.reserve(n);

            const size_t end =
                write_idx_.load(std::memory_order_relaxed);
            const size_t start =
                (end + LATENCY_BUF_SIZE - n) % LATENCY_BUF_SIZE;

            for (size_t i = 0; i < n; ++i) {
                const size_t idx = (start + i) % LATENCY_BUF_SIZE;
                const long long value =
                    apply_latencies_[idx].load(std::memory_order_relaxed);
                if (value >= 0)
                    samples.push_back(value);
            }

            if (!samples.empty()) {
                std::sort(samples.begin(), samples.end());
                s.p50_latency_us = percentile(samples, 0.50) / 1000.0;
                s.p99_latency_us = percentile(samples, 0.99) / 1000.0;
                s.latency_samples = samples.size();
            }
        }

        // ---------------- END-TO-END LATENCY ----------------
        const uint64_t e2e_total =
            e2e_latency_count_.load(std::memory_order_relaxed);

        const size_t e2e_n =
            static_cast<size_t>(
                std::min<uint64_t>(e2e_total, E2E_LATENCY_BUF_SIZE));

        if (e2e_n > 0) {
            std::vector<long long> e2e_samples;
            e2e_samples.reserve(e2e_n);

            const size_t e2e_end =
                e2e_write_idx_.load(std::memory_order_relaxed);
            const size_t e2e_start =
                (e2e_end + E2E_LATENCY_BUF_SIZE - e2e_n) % E2E_LATENCY_BUF_SIZE;

            for (size_t i = 0; i < e2e_n; ++i) {
                const size_t idx = (e2e_start + i) % E2E_LATENCY_BUF_SIZE;
                const long long value =
                    e2e_latencies_[idx].load(std::memory_order_relaxed);
                if (value >= 0)
                    e2e_samples.push_back(value);
            }

            if (!e2e_samples.empty()) {
                std::sort(e2e_samples.begin(), e2e_samples.end());
                s.e2e_p50_us = percentile(e2e_samples, 0.50) / 1000.0;
                s.e2e_p99_us = percentile(e2e_samples, 0.99) / 1000.0;
                s.e2e_latency_samples = e2e_samples.size();
            }
        }

        // ---------------- PARSE LATENCY ----------------
        const uint64_t parse_total =
            parse_latency_count_.load(std::memory_order_relaxed);

        const size_t parse_n =
            static_cast<size_t>(
                std::min<uint64_t>(parse_total, PARSE_LATENCY_BUF_SIZE));

        if (parse_n > 0) {
            std::vector<long long> parse_samples;
            parse_samples.reserve(parse_n);

            const size_t parse_end =
                parse_write_idx_.load(std::memory_order_relaxed);
            const size_t parse_start =
                (parse_end + PARSE_LATENCY_BUF_SIZE - parse_n) % PARSE_LATENCY_BUF_SIZE;

            for (size_t i = 0; i < parse_n; ++i) {
                const size_t idx = (parse_start + i) % PARSE_LATENCY_BUF_SIZE;
                const long long value =
                    parse_latencies_[idx].load(std::memory_order_relaxed);
                if (value >= 0)
                    parse_samples.push_back(value);
            }

            if (!parse_samples.empty()) {
                std::sort(parse_samples.begin(), parse_samples.end());
                s.parse_p50_us = percentile(parse_samples, 0.50) / 1000.0;
                s.parse_p99_us = percentile(parse_samples, 0.99) / 1000.0;
                s.parse_latency_samples = parse_samples.size();
            }
        }

        return s;
    }
    // -------------------------------------------------------------------------
    // Printing
    // -------------------------------------------------------------------------

    void printSnapshot(std::ostream& os = std::cout) const {
        const Snapshot s = snapshot();
        os << std::fixed << std::setprecision(2) << "[" << name_ << "] "
        << "\n========== Metrics Snapshot ==========\n"
        << "Deltas applied          : " << s.delta_applied << '\n'
        << "Validation success      : " << s.validation_success << '\n'
        << "Validation mismatches   : " << s.validation_mismatch << '\n'
        << "Validation skipped      : " << s.validation_skipped << '\n'
        << "Resyncs triggered       : " << s.resync_triggered << '\n'

        << "Active bids/asks        : "
        << s.active_bids << " / " << s.active_asks << '\n'

        << "Apply latency samples   : " << s.latency_samples << '\n'
        << "Apply latency p50/p99   : "
        << s.p50_latency_us << "us / "
        << s.p99_latency_us << "us\n"

        << "Parse latency samples   : " << s.parse_latency_samples << '\n'
        << "Parse latency p50/p99   : "
        << s.parse_p50_us << "us / "
        << s.parse_p99_us << "us\n"
        << "E2E latency samples     : " << s.e2e_latency_samples << '\n'
        << "E2E latency p50/p99     : "
        << s.e2e_p50_us << "us / "
        << s.e2e_p99_us << "us\n"

        << "=======================================\n";

    }


std::string formatSnapshot() const {
    auto s = snapshot();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << "[" << name_ << "] "
        << "\n========== Metrics Snapshot ==========\n"
        << "Deltas applied          : " << s.delta_applied << '\n'
        << "Validation success      : " << s.validation_success << '\n'
        << "Validation mismatches   : " << s.validation_mismatch << '\n'
        << "Validation skipped      : " << s.validation_skipped << '\n'
        << "Resyncs triggered       : " << s.resync_triggered << '\n'

        << "Active bids/asks        : "
        << s.active_bids << " / " << s.active_asks << '\n'

        << "Apply latency samples   : " << s.latency_samples << '\n'
        << "Apply latency p50/p99   : "
        << s.p50_latency_us << "us / "
        << s.p99_latency_us << "us\n"

        << "Parse latency samples   : " << s.parse_latency_samples << '\n'
        << "Parse latency p50/p99   : "
        << s.parse_p50_us << "us / "
        << s.parse_p99_us << "us\n"

        << "E2E latency samples     : " << s.e2e_latency_samples << '\n'
        << "E2E latency p50/p99     : "
        << s.e2e_p50_us << "us / "
        << s.e2e_p99_us << "us\n"

        << "=======================================\n";

    return oss.str();
}


    void startBenchmark() { 
        start_time_ = std::chrono::steady_clock::now(); 
    }


    double throughput() const {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time_).count();
        return elapsed > 0 ? static_cast<double>(delta_applied_.load()) / elapsed : 0;
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
    std::string name_; 

    std::array<std::atomic<long long>, LATENCY_BUF_SIZE>
        apply_latencies_{};

    std::atomic<size_t> write_idx_{0};

    // Lifetime number of recorded latency observations.
    std::atomic<uint64_t> latency_count_{0};

    // -------------------------------------------------------------------------
    // Counters
    // -------------------------------------------------------------------------

    // std::atomic<uint64_t> delta_applied_{0};

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


    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> delta_applied_{0};
    


    // e2e latency calculation. 
    static constexpr size_t E2E_LATENCY_BUF_SIZE = LATENCY_BUF_SIZE; // reuse same size
    std::array<std::atomic<long long>, E2E_LATENCY_BUF_SIZE> e2e_latencies_;
    std::atomic<size_t> e2e_write_idx_{0};
    std::atomic<uint64_t> e2e_latency_count_{0};

    // parse metrics calculation. 
    static constexpr size_t PARSE_LATENCY_BUF_SIZE = LATENCY_BUF_SIZE; // reuse same size
    std::array<std::atomic<long long>, PARSE_LATENCY_BUF_SIZE> parse_latencies_;
    std::atomic<size_t> parse_write_idx_{0};
    std::atomic<uint64_t> parse_latency_count_{0};

    // Delete from here

// Till here 


};