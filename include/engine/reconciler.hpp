#pragma once

#include "orderbook/orderbook.hpp"
#include "networking/rest_client.hpp"
#include "networking/websocket_client.hpp"
#include "engine/spscringqueue.hpp"
#include "utils/symbol_info.hpp"
#include "utils/metrics.hpp"
#include "utils/price_parser.hpp"

#include "engine/diff_history_buffer.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <string>
#include <cstdint>

// One raw WebSocket frame handed from the IO thread to the engine.
//
// `buf` holds the frame in [0, len) followed by at least SIMDJSON_PADDING zero
// bytes, so the engine parses it in place (no copy). Buffers are recycled:
// the engine returns each one through FreeBufferQueue once parsed, and the IO
// thread reuses it for a later frame, so neither side allocates per message.
struct QueuedDelta
{
    std::string buf;
    size_t len = 0;
    std::chrono::steady_clock::time_point wire_time;
};

using LiveQueue = SpscRingQueue<QueuedDelta, 8192>;

// Engine -> IO thread: empty frame buffers ready for reuse (capacity retained).
using FreeBufferQueue = SpscRingQueue<std::string, 8192>;
class Reconciler
{
public:
    Reconciler(OrderBook &book,
               RestClient &rest,
               WebSocketClient &ws,
               LiveQueue &queue,
               DiffHistoryBuffer &history,
               MetricsCollector &metrics,
               size_t window_size = 1'000'000,
               std::chrono::milliseconds feed_stall_timeout = std::chrono::seconds(15));

    // No copying or moving.
    Reconciler(const Reconciler &) = delete;
    Reconciler &operator=(const Reconciler &) = delete;
    Reconciler(Reconciler &&) = delete;
    Reconciler &operator=(Reconciler &&) = delete;

    void run(const std::string &symbol);
    void stop();

    // --- Validator interface ---
    bool waitUntilInitialized(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    const SymbolInfo &symbolInfo() const { return symbol_info_; }
    uint64_t getLastAppliedSeq() const { return lastAppliedSeq_.load(std::memory_order_acquire); }
    bool isSyncing() const { return syncing_.load(std::memory_order_acquire); }

    void triggerResync();

    // --- Lock-free freeze (Validator -> engine) ---
    // The engine thread is the only thread that touches the live book. To get
    // a consistent copy, the Validator posts a request and the engine fills
    // it between messages, when its queue is empty. Returns false on timeout
    // (e.g. the engine is busy bootstrapping) or shutdown.
    struct FrozenBook
    {
        OrderBook::ActiveSnapshot levels;
        uint64_t seq = 0;          // lastAppliedSeq the copy reflects
        int64_t copy_ns = 0;       // time the engine spent copying

        // Price range the live book can know completely. The bootstrap REST
        // snapshot lists only the top kSnapshotDepth levels per side, so a
        // level deeper than that which never changes afterwards is never
        // sent to us. Bids are complete at/above known_bid_floor, asks
        // at/below known_ask_ceiling.
        int64_t known_bid_floor = 0;
        int64_t known_ask_ceiling = INT64_MAX;
    };

    static constexpr int kSnapshotDepth = 5000;  // REST /depth limit used for bootstrap
    bool requestFreeze(FrozenBook &out, std::chrono::milliseconds timeout);
    void verifyBootstrapSnapshot(const std::string &checkpoint,
                                 const OrderBook::ActiveSnapshot &live_snap);

private:
    // Private methods
    bool bootstrap(const std::string &symbol);
    bool drainPendingBuffer(uint64_t snapshot_id, int tick_decimals, int qty_decimals);
    void processLiveDelta(const QueuedDelta &msg);
    void serviceFreezeRequest();  // engine thread only
    void recycleBuffer(QueuedDelta &msg);
    std::string buildWebSocketTarget(const std::string &symbol) const;
    void fetchSymbolInfo(const std::string &symbol);

    // ---- Members ----
    OrderBook &book_;
    RestClient &rest_;
    WebSocketClient &ws_;
    LiveQueue &queue_;
    FreeBufferQueue free_buffers_;  // engine produces, IO thread consumes
    DiffHistoryBuffer &history_;
    MetricsCollector &metrics_;
    // SymbolParseConfig& parse_config_;

    // SINGLE SOURCE OF TRUTH for symbol metadata.
    SymbolInfo symbol_info_;
    bool is_initialized_ = false; // simple flag, not atomic
    std::string current_symbol_;  // stored for logging / resync

    size_t window_size_ = 1'000'000;
    std::condition_variable init_cv_;
    std::mutex init_mutex_;

    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> lastAppliedSeq_{0};
    std::atomic<bool> resync_requested_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> syncing_{false};

    std::atomic<uint64_t> resync_epoch_{0};

    // Feed-stall detection: no WebSocket message for this long while live
    // means the stream is dead even if the socket still looks open.
    std::chrono::milliseconds feed_stall_timeout_;
    std::atomic<int64_t> last_rx_ns_{0};  // steady_clock ns of the last frame (IO thread writes)

    // Set by bootstrap(), read when serving a freeze (both engine thread).
    int64_t known_bid_floor_ = 0;
    int64_t known_ask_ceiling_ = INT64_MAX;

    // Freeze request handshake (see requestFreeze).
    std::atomic<bool> freeze_pending_{false};  // polled by the engine when idle
    std::mutex freeze_mutex_;
    std::condition_variable freeze_cv_;
    FrozenBook *freeze_target_ = nullptr;      // guarded by freeze_mutex_
    bool freeze_done_ = false;                 // guarded by freeze_mutex_

    // Temporary diagnostic: snapshot vectors from last bootstrap
    // std::vector<std::pair<int64_t, uint64_t>> last_bootstrap_bids_;
    // std::vector<std::pair<int64_t, uint64_t>> last_bootstrap_asks_;
};
void prefaultEngineThreadLocals();