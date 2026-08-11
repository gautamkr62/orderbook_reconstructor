#pragma once

#include "orderbook/orderbook.hpp"
#include "networking/rest_client.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/reconciler.hpp"
#include "utils/metrics.hpp"


#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * ShadowResult – carries the shadow book, its sequence, and the depths to compare.
 */
struct ShadowResult {
    std::unique_ptr<OrderBook> book;
    uint64_t snapshot_seq = 0;
    size_t bid_depth = 0;   // number of bids returned by snapshot
    size_t ask_depth = 0;   // number of asks returned by snapshot
};

/**
 * Validator – Background correctness auditor.
 *
 * Periodically checks that the live OrderBook matches the exchange's published
 * depth. Since Binance REST only returns the top N levels (up to 5000), this
 * validator compares only the portion of the book that the exchange exposes.
 *
 * Threading:
 *   - Runs on its own thread.
 *   - Reads the live book under a shared_lock during freeze.
 *   - Never modifies the live book.
 */
class Validator {
public:
    Validator(OrderBook& live_book,
              DiffHistoryBuffer& history,
              RestClient& rest,
              Reconciler& reconciler,
              MetricsCollector& metrics,
              size_t window_size = 1'000'000,
              std::chrono::seconds interval = std::chrono::seconds(60));

    ~Validator();

    void run();
    void stop();

    // Metrics
    uint64_t successCount() const { return success_count_.load(std::memory_order_acquire); }
    uint64_t mismatchCount() const { return mismatch_count_.load(std::memory_order_acquire); }
    uint64_t resyncsTriggered() const { return resyncs_triggered_.load(std::memory_order_acquire); }
    std::chrono::system_clock::time_point lastValidationTime() const;

private:
    void updateTimestamp();
    bool performValidation();
    bool freeze(uint64_t& F,
                std::vector<std::pair<int64_t, uint64_t>>& frozen_bids,
                std::vector<std::pair<int64_t, uint64_t>>& frozen_asks);
    bool buildShadowBook(ShadowResult& result);
    bool parseSnapshot(const std::string& json,
                       uint64_t& snapshot_seq,
                       std::vector<std::pair<int64_t, uint64_t>>& parsed_bids,
                       std::vector<std::pair<int64_t, uint64_t>>& parsed_asks);
    bool replayHistory(uint64_t S, uint64_t F, OrderBook& shadow);
    // bool compareBooks(const OrderBook& shadow,
    //                   const std::vector<std::pair<int64_t, uint64_t>>& frozen_bids,
    //                   const std::vector<std::pair<int64_t, uint64_t>>& frozen_asks,
    //                   size_t bid_depth,
    //                   size_t ask_depth,
    //                   std::string& mismatch_details);   // <-- updated signature

    // Members
    OrderBook& live_book_;
    DiffHistoryBuffer& history_;
    RestClient& rest_;
    Reconciler& reconciler_;
    MetricsCollector& metrics_;


    size_t window_size_;
    std::chrono::seconds interval_;

    std::atomic<bool> running_{true};
    std::mutex cv_mutex_;
    std::condition_variable cv_;

    std::atomic<uint64_t> success_count_{0};
    std::atomic<uint64_t> mismatch_count_{0};
    std::atomic<uint64_t> resyncs_triggered_{0};
    std::atomic<int64_t> last_validation_timestamp_ms_{0};
    size_t validation_attempts_ = 0;
    size_t bid_tail_drift_attempts_ = 0;
    size_t ask_tail_drift_attempts_ = 0;

    size_t max_bid_tail_drift_ = 0;
    size_t max_ask_tail_drift_ = 0;
};