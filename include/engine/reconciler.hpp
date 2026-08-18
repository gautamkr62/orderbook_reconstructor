#pragma once

#include "orderbook/orderbook.hpp"
#include "networking/rest_client.hpp"
#include "networking/websocket_client.hpp"
#include "engine/threadsafe_queue.hpp"
#include "engine/diff_history_buffer.hpp"
#include "utils/symbol_info.hpp"
#include "utils/metrics.hpp"

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <string>

class Reconciler {
public:
  

    Reconciler(OrderBook& book,
        RestClient& rest,
        WebSocketClient& ws,
        ThreadSafeQueue<std::string>& queue,
        DiffHistoryBuffer& history,
        MetricsCollector& metrics, 
        size_t window_size = 1'000'000);


    // No copying or moving.
    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;
    Reconciler(Reconciler&&) = delete;
    Reconciler& operator=(Reconciler&&) = delete;

    void run(const std::string& symbol);
    void stop();

    // --- Validator interface ---
    bool waitUntilInitialized(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    const SymbolInfo& symbolInfo() const { return symbol_info_; }
    uint64_t getLastAppliedSeq() const { return lastAppliedSeq_.load(std::memory_order_acquire); }
    bool isSyncing() const { return syncing_.load(std::memory_order_acquire); }

    void triggerResync();
    void verifyBootstrapSnapshot(const std::string& checkpoint,
                             const OrderBook::ActiveSnapshot& live_snap);


private:
    // Private methods
    bool bootstrap(const std::string& symbol);
    bool drainPendingBuffer(uint64_t snapshot_id, int tick_decimals, int qty_decimals);
    void processLiveDelta(const std::string& raw_msg);
    std::string buildWebSocketTarget(const std::string& symbol) const;
    void fetchSymbolInfo(const std::string& symbol);

    
    
    // ---- Members ----
    OrderBook& book_;
    RestClient& rest_;
    WebSocketClient& ws_;
    ThreadSafeQueue<std::string>& queue_;
    DiffHistoryBuffer& history_;
    MetricsCollector& metrics_;

    // SINGLE SOURCE OF TRUTH for symbol metadata.
    SymbolInfo symbol_info_;
    bool is_initialized_ = false;        // simple flag, not atomic
    std::string current_symbol_;         // stored for logging / resync


    size_t window_size_ = 1'000'000;
    std::condition_variable init_cv_;
    std::mutex init_mutex_;
    
    
    
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> lastAppliedSeq_{0};
    std::atomic<bool> resync_requested_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> syncing_{false};

    std::atomic<uint64_t> resync_epoch_{0};

    // Temporary diagnostic: snapshot vectors from last bootstrap
// std::vector<std::pair<int64_t, uint64_t>> last_bootstrap_bids_;
// std::vector<std::pair<int64_t, uint64_t>> last_bootstrap_asks_;

};