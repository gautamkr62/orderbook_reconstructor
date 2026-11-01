#include "engine/reconciler.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/delta.hpp"
#include "utils/price_parser.hpp"
#include "utils/metrics.hpp"
#include "engine/apply_delta.hpp"
#include "utils/json_levels.hpp"
#include "engine/delta_parser.hpp"


#include <boost/algorithm/string.hpp>

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cctype>
#include <atomic>

// ---------------------------------------------------------------------
// RAII helper: ScopedSync
// ---------------------------------------------------------------------
class ScopedSync
{
public:
    explicit ScopedSync(std::atomic<bool> &flag) : flag_(flag)
    {
        flag_.store(true, std::memory_order_release);
    }

    ~ScopedSync()
    {
        flag_.store(false, std::memory_order_release);
    }

    ScopedSync(const ScopedSync &) = delete;
    ScopedSync &operator=(const ScopedSync &) = delete;

private:
    std::atomic<bool> &flag_;
};

// ---------------------------------------------------------------------
// Helper: interruptible sleep
// ---------------------------------------------------------------------
static void interruptibleSleep(std::chrono::milliseconds duration,
                               const std::atomic<bool> &running)
{
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
    auto elapsed = std::chrono::milliseconds(0);
    while (elapsed < duration && running.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(POLL_INTERVAL);
        elapsed += POLL_INTERVAL;
    }
}

namespace {

    // ---------------------------------------------------------------------
    // Helper: count significant decimals
    // ---------------------------------------------------------------------
    static int countSignificantDecimals(std::string_view str)
    {
        size_t dot = str.find('.');
        if (dot == std::string_view::npos)
            return 0;
        size_t end = str.size() - 1;
        while (end > dot && str[end] == '0')
            --end;
        if (end == dot)
            return 0;
        return static_cast<int>(end - dot);
    }

} // unnamed namespace


void prefaultEngineThreadLocals() {
    prefaultDeltaParser(2, 5);
}

// ---------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------
Reconciler::Reconciler(OrderBook &book,
                       RestClient &rest,
                       WebSocketClient &ws,
                       LiveQueue& queue,
                       DiffHistoryBuffer &history,
                       MetricsCollector &metrics,
                       size_t window_size,
                       std::chrono::milliseconds feed_stall_timeout)
    : book_(book), rest_(rest), ws_(ws), queue_(queue), history_(history), metrics_(metrics), window_size_(window_size),
      feed_stall_timeout_(feed_stall_timeout) {}

// ---------------------------------------------------------------------
// fetchSymbolInfo – Fills symbol metadata
// ---------------------------------------------------------------------
void Reconciler::fetchSymbolInfo(const std::string &symbol)
{
    RestResponse response = rest_.fetchExchangeInfo(symbol);
    if (!response.ok())
    {
        throw std::runtime_error("fetchSymbolInfo: failed to fetch exchangeInfo for " + symbol);
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc_result = parser.iterate(padded);
    if (doc_result.error())
    {
        throw std::runtime_error("fetchSymbolInfo: failed to parse exchangeInfo JSON");
    }
    auto doc = std::move(doc_result).value();

    auto symbols = doc["symbols"].get_array();
    bool found = false;
    for (auto sym : symbols)
    {
        std::string_view sym_name = sym["symbol"].get_string();
        if (sym_name != symbol)
            continue;

        found = true;
        auto filters = sym["filters"].get_array();
        for (auto filter : filters)
        {
            std::string_view filter_type = filter["filterType"].get_string();
            if (filter_type == "PRICE_FILTER")
            {
                std::string_view tick_size = filter["tickSize"].get_string();
                symbol_info_.tick_decimals = countSignificantDecimals(tick_size);
            }
            else if (filter_type == "LOT_SIZE")
            {
                std::string_view step_size = filter["stepSize"].get_string();
                symbol_info_.qty_decimals = countSignificantDecimals(step_size);
            }
        }
        break;
    }

    if (!found)
    {
        throw std::runtime_error("fetchSymbolInfo: symbol " + symbol + " not found in exchangeInfo");
    }

    symbol_info_.symbol = symbol;
    // std::cout << "[SymbolInfo] " << symbol
    //           << " tick_decimals=" << symbol_info_.tick_decimals
    //           << " qty_decimals=" << symbol_info_.qty_decimals << std::endl;
}

// ---------------------------------------------------------------------
// Public run()
// ---------------------------------------------------------------------
void Reconciler::run(const std::string &symbol)
{

    // -----------------------------------------------------------------
    // 0. PRE-FILL the frame-buffer pool (engine is its only producer).
    //    The IO thread allocates only if the pool ever runs dry.
    // -----------------------------------------------------------------
    constexpr size_t kPrefilledFrameBuffers = 256;
    for (size_t i = 0; i < kPrefilledFrameBuffers; ++i)
    {
        std::string buf;
        buf.resize(kMinFrameBuffer);
        free_buffers_.tryPush(std::move(buf));
    }

    // -----------------------------------------------------------------
    // 1. FETCH SYMBOL METADATA
    // -----------------------------------------------------------------
    while (running_.load(std::memory_order_acquire))
    {
        try
        {
            fetchSymbolInfo(symbol);
            break;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Reconciler] Failed to fetch symbol info: " << e.what()
                      << ". Retrying in 2s..." << std::endl;
            interruptibleSleep(std::chrono::seconds(2), running_);
        }
    }
    if (!running_.load())
        return;

    // -----------------------------------------------------------------
    // 1b. NOW warm simdjson with real decimal values
    // -----------------------------------------------------------------
    prefaultDeltaParser(symbol_info_.tick_decimals, symbol_info_.qty_decimals);



    // -----------------------------------------------------------------
    // 2. SET WEBSOCKET CALLBACK (I/O THREAD)
    // -----------------------------------------------------------------
    ws_.setMessageCallback(
        [this](std::string_view frame)
        {
            const auto wire_time = std::chrono::steady_clock::now();
            last_rx_ns_.store(wire_time.time_since_epoch().count(), std::memory_order_relaxed);
            QueuedDelta item;
            free_buffers_.tryPop(item.buf);  // empty pool -> fillFrameBuffer allocates
            fillFrameBuffer(item.buf, frame);
            item.len = frame.size();
            item.wire_time = wire_time;
            if (!queue_.tryPush(std::move(item)))
            {
                std::cerr << "WARNING: Queue full, dropping WebSocket message." << std::endl;
            }
        });

    // Runs on the IO thread whenever the connection ends: remote close,
    // reset, TLS error, or the idle-ping timeout declaring it dead.
    // Closes that bootstrap() initiates itself (syncing_) or shutdown
    // causes are expected and ignored; any other close means the live
    // book is no longer being updated.
    ws_.setCloseCallback(
        [this]()
        {
            if (!running_.load(std::memory_order_acquire) ||
                syncing_.load(std::memory_order_acquire))
                return;
            std::cerr << "[Engine] WebSocket closed unexpectedly. Triggering resync." << std::endl;
            resync_requested_.store(true, std::memory_order_release);
        });

    // -----------------------------------------------------------------
    // 3. BOOTSTRAP LOOP (connect + snapshot + drain)
    // -----------------------------------------------------------------
    while (running_.load(std::memory_order_acquire))
    {
        if (bootstrap(symbol_info_.symbol))
            break;
        std::cerr << "Bootstrap failed. Retrying in 2s..." << std::endl;
        interruptibleSleep(std::chrono::seconds(2), running_);
    }
    if (!running_.load())
        return;

    // -----------------------------------------------------------------
    // 4. SIGNAL INITIALIZATION
    // -----------------------------------------------------------------
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        initialized_.store(true, std::memory_order_release);
    }
    init_cv_.notify_all();

    // -----------------------------------------------------------------
    // 5. STEADY-STATE LOOP
    // -----------------------------------------------------------------
    while (running_.load(std::memory_order_acquire))
    {
        // -------- Check for dropped messages -------------------------
        uint64_t dropped = queue_.droppedCount();
        if (dropped > 0)
        {
            std::cerr << "[Engine] Detected " << dropped
                      << " dropped WebSocket messages. Triggering resync." << std::endl;
            queue_.resetDroppedCount();
            resync_requested_.store(true, std::memory_order_release);
            continue; // let resync block handle it next iteration
        }

        // -------- Check for resync request ---------------------------
        if (resync_requested_.exchange(false))
        {
            std::cout << "[Engine] Re-sync triggered. Restarting bootstrap..." << std::endl;

            // Update global metrics (this was missing previously)
            metrics_.incResyncTriggered();

            while (running_.load(std::memory_order_acquire))
            {
                if (bootstrap(symbol_info_.symbol))
                    break;
                std::cerr << "Re-sync bootstrap failed. Retrying..." << std::endl;
                interruptibleSleep(std::chrono::seconds(2), running_);
            }
            if (!running_.load())
                break;

            std::cout << "[Engine] Re-sync complete. lastAppliedSeq="
                      << lastAppliedSeq_.load() << std::endl;
            continue;
        }

        // -------- Process messages -----------------------------------
        QueuedDelta msg;
        const int64_t stall_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(feed_stall_timeout_).count();
        uint32_t idle_spins = 0;
        while (running_.load(std::memory_order_acquire))
        {
            while (queue_.tryPop(msg))
            {
                try
                {
                    processLiveDelta(msg);
                }
                catch (const std::exception &e)
                {
                    // A bad frame must not kill the engine thread and freeze the book.
                    std::cerr << "[Engine] Failed to process message: " << e.what()
                              << ". Triggering resync." << std::endl;
                    resync_requested_.store(true, std::memory_order_release);
                }
                recycleBuffer(msg);

                if (!running_.load(std::memory_order_relaxed))
                    break;

                if (resync_requested_.load(std::memory_order_acquire) ||
                    queue_.droppedCount() > 0)
                    break;
            }

            if (resync_requested_.load(std::memory_order_acquire) ||
                queue_.droppedCount() > 0)
                break;

            // Queue empty: this is the moment to hand the Validator its copy.
            if (freeze_pending_.load(std::memory_order_acquire))
                serviceFreezeRequest();

            // Queue empty: every few thousand spins, check the feed is alive.
            if (++idle_spins >= 4096)
            {
                idle_spins = 0;
                const int64_t now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
                const int64_t silent_ns = now_ns - last_rx_ns_.load(std::memory_order_relaxed);
                if (silent_ns > stall_ns)
                {
                    std::cerr << "[Engine] Feed stalled: no WebSocket message for "
                              << silent_ns / 1'000'000 << " ms. Triggering resync." << std::endl;
                    resync_requested_.store(true, std::memory_order_release);
                    break;
                }
            }

            SPSC_CPU_PAUSE();  // _mm_pause on x86, yield on ARM64
        }
    }

    // (optional) final log
    std::cout << "[Engine] Reconciler stopped." << std::endl;
}



// ---------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------
bool Reconciler::bootstrap(const std::string &symbol)
{

    uint64_t epoch = resync_epoch_.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[EPOCH] Starting bootstrap epoch=" << epoch << std::endl;
    ScopedSync syncing(syncing_);
    // This bootstrap is the resync: drop requests raised before it started
    // (e.g. a late close notification from the previous connection).
    resync_requested_.store(false, std::memory_order_release);

    try
    {
        std::cout << "[Bootstrap] Starting for " << symbol << std::endl;

        {
            std::lock_guard<std::mutex> lock(init_mutex_);
            initialized_.store(false, std::memory_order_release);
        }

        // Close existing WebSocket if needed
        if (ws_.isConnected() || ws_.isConnecting())
        {
            std::cout << "[Bootstrap] Closing existing WebSocket..." << std::endl;
            ws_.disconnect();
            auto res = ws_.waitForClose(std::chrono::seconds(5));
            if (res == CloseResult::Timeout)
            {
                ws_.forceClose();
                ws_.waitForClose(std::chrono::seconds(2));
            }
            if (!running_.load())
                return false;
        }

        // Connect WebSocket
        std::string target = buildWebSocketTarget(symbol);
        std::cout << "[Bootstrap] Connecting to " << target << std::endl;
        ws_.connectAsync(target);

        auto res = ws_.waitForConnected(std::chrono::seconds(5));
        if (res != CloseResult::Closed || !running_.load())
        {
            std::cerr << "[Bootstrap] WebSocket connection failed or timeout." << std::endl;
            ws_.forceClose();
            return false;
        }
        std::cout << "[Bootstrap] WebSocket connected." << std::endl;

        // Fetch REST snapshot
        RestResponse snapshot_res = rest_.fetchDepthSnapshot(symbol, kSnapshotDepth);
        if (!snapshot_res.ok())
        {
            std::cerr << "[Bootstrap] Snapshot fetch failed." << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        // Parse snapshot
        thread_local simdjson::ondemand::parser parser;
        auto doc_result = parser.iterate(snapshot_res.body);
        if (doc_result.error())
        {
            std::cerr << "[Bootstrap] Failed to parse snapshot JSON." << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }
        auto doc = std::move(doc_result).value();

        uint64_t S = doc["lastUpdateId"];
        std::vector<std::pair<int64_t, uint64_t>> parsed_bids;
        std::vector<std::pair<int64_t, uint64_t>> parsed_asks;

        // Parse bids
        auto bids_array = doc["bids"].get_array();
        parsed_bids.reserve(bids_array.count_elements());
        int64_t best_bid_tick = -1;
        for (auto bid : bids_array)
        {
            std::string_view price, qty;
            readPriceQty(bid, price, qty);
            int64_t tick = parsePriceToTick(price, symbol_info_.tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty, symbol_info_.qty_decimals);
            parsed_bids.emplace_back(tick, qty_scaled);
            if (best_bid_tick == -1)
                best_bid_tick = tick;
        }

        // Parse asks
        auto asks_array = doc["asks"].get_array();
        parsed_asks.reserve(asks_array.count_elements());
        int64_t best_ask_tick = -1;
        for (auto ask : asks_array)
        {
            std::string_view price, qty;
            readPriceQty(ask, price, qty);
            int64_t tick = parsePriceToTick(price, symbol_info_.tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty, symbol_info_.qty_decimals);
            parsed_asks.emplace_back(tick, qty_scaled);
            if (best_ask_tick == -1)
                best_ask_tick = tick;
        }

        if (parsed_bids.empty() || parsed_asks.empty())
        {
            std::cerr << "[Bootstrap] Snapshot has empty levels." << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        {
            // Engine thread owns the live book: no lock needed.
            // Preserve any growth from the previous window (avoid resetting to
            // the default size after a resync).
            size_t current_size = book_.currentWindowSize();
            size_t new_window_size = std::max(window_size_, current_size);

            book_.reset(best_bid_tick, best_ask_tick, new_window_size);
            book_.applySnapshot(parsed_bids, parsed_asks);
            lastAppliedSeq_.store(S, std::memory_order_release);

            // A side shorter than the requested depth is the whole side.
            known_bid_floor_ = parsed_bids.size() >= static_cast<size_t>(kSnapshotDepth)
                                   ? parsed_bids.back().first : 0;
            known_ask_ceiling_ = parsed_asks.size() >= static_cast<size_t>(kSnapshotDepth)
                                     ? parsed_asks.back().first : INT64_MAX;
        }

        // Drain buffered deltas and bridge to snapshot
        bool success = drainPendingBuffer(S, symbol_info_.tick_decimals, symbol_info_.qty_decimals);
        if (!success)
        {
            std::cerr << "[Bootstrap] Failed during buffer drain. Clearing queue." << std::endl;
            queue_.clear();
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        // Feed is live as of now: restart the stall clock.
        last_rx_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                          std::memory_order_relaxed);

        // Mark initialized
        {
            std::lock_guard<std::mutex> lock(init_mutex_);
            initialized_.store(true, std::memory_order_release);
        }
        init_cv_.notify_all();

        std::cout << "[Bootstrap] Success. final lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Bootstrap] Exception: " << e.what() << std::endl;
        ws_.disconnect();
        ws_.waitForClose(std::chrono::seconds(2));
        return false;
    }
    catch (...)
    {
        std::cerr << "[Bootstrap] Unknown exception." << std::endl;
        ws_.disconnect();
        ws_.waitForClose(std::chrono::seconds(2));
        return false;
    }
}

// ---------------------------------------------------------------------
// drainPendingBuffer
// ---------------------------------------------------------------------

// This updates the rest snapshot for the first time. 
bool Reconciler::drainPendingBuffer(uint64_t snapshot_id,
                                    int tick_decimals,
                                    int qty_decimals)
{
    uint64_t seq = snapshot_id;
    bool bridged = false;

    constexpr auto WAIT_INTERVAL = std::chrono::milliseconds(10);
    constexpr auto WAIT_TIMEOUT = std::chrono::seconds(5);
    const auto deadline = std::chrono::steady_clock::now() + WAIT_TIMEOUT;

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto maybe_msg = queue_.tryPop();

        if (!maybe_msg.has_value())
        {
            if (bridged)
            {
                break;
            }
            std::this_thread::sleep_for(WAIT_INTERVAL);
            continue;
        }

        thread_local Delta delta;

        if (delta.bids.capacity() < 2048)
            delta.bids.reserve(2048);

        if (delta.asks.capacity() < 2048)
            delta.asks.reserve(2048);

        parseDelta(
            maybe_msg->buf,
            maybe_msg->len,
            tick_decimals,
            qty_decimals,
            delta
        );
        recycleBuffer(*maybe_msg);  // frame fully parsed into delta

        const uint64_t U = delta.U;
        const uint64_t u = delta.u;

        if (u <= seq)
        {
            continue;
        }

        if (!bridged)
        {
            if (U <= seq + 1 && u >= seq + 1)
            {
                bridged = true;
            }
            else if (U > seq + 1)
            {
                return false;
            }
            else
            {
                return false;
            }
        }
        else
        {
            if (U != seq + 1)
            {
                return false;
            }
        }

        applyDelta(delta, book_);
        seq = u;
        lastAppliedSeq_.store(seq, std::memory_order_release);
        history_.push(delta);  // copies into the ring; delta keeps its capacity
    }

    if (!bridged)
    {
        std::cerr << "[DRAIN] Timed out waiting for bridge event. snapshot="
                  << snapshot_id << std::endl;
        return false;
    }

    // std::cout << "[DRAIN] Successfully bridged snapshot. final seq=" << seq << std::endl;
    return true;
}

// ---------------------------------------------------------------------
// processLiveDelta
// ---------------------------------------------------------------------


// Hand a parsed frame's buffer back to the IO thread (keeps its capacity).
void Reconciler::recycleBuffer(QueuedDelta& msg)
{
    free_buffers_.tryPush(std::move(msg.buf));  // pool full -> buffer is simply freed
}

void Reconciler::processLiveDelta(const QueuedDelta& msg)
{
    // ---------------------------
    // 1. Parse
    // ---------------------------
    const auto parse_start = std::chrono::steady_clock::now();
    // Reused per-thread Delta: history_.push() copies out of it, so the
    // reserve below allocates only on the first call.
    thread_local Delta delta;
    if (delta.bids.capacity() < 2048) delta.bids.reserve(2048);
    if (delta.asks.capacity() < 2048) delta.asks.reserve(2048);

    parseDelta(
        msg.buf,
        msg.len,
        symbol_info_.tick_decimals,
        symbol_info_.qty_decimals,
        delta
    );


    const auto parse_end = std::chrono::steady_clock::now();
    // std::cout<<"parse_end - parse start time -> "<<parse_end - parse_start<< "\n"; 
    metrics_.recordParseLatency(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        parse_end - parse_start));

    // ---------------------------
    // 2. Sequence checks
    // ---------------------------
    const uint64_t U = delta.U;
    const uint64_t u = delta.u;
    const uint64_t seq = lastAppliedSeq_.load(std::memory_order_acquire);

    if (u <= seq) {
        // Stale / duplicate
        return;
    }

    if (U != seq + 1) {
        // Gap or overlap in steady state
        resync_requested_ = true;
        return;
    }

    // ---------------------------
    // 3. Apply — the engine thread owns the live book, so no lock: the
    //    Validator gets its copy through requestFreeze() between messages.
    // ---------------------------
    const auto book_start = std::chrono::steady_clock::now();
    applyDelta(delta, book_);
    const auto book_end = std::chrono::steady_clock::now();

    lastAppliedSeq_.store(u, std::memory_order_release);

    const auto critical_end = std::chrono::steady_clock::now();

    // ---------------------------
    // 4. History & metrics
    // ---------------------------
    history_.push(delta);   // memcpy into the preallocated ring — no allocation

    // End-to-end: wire → book updated (excludes history push)
    metrics_.recordE2ELatency(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            book_end - msg.wire_time));

    // Critical section: apply + seq store (no lock any more)
    metrics_.recordCriticalSectionLatency(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            critical_end - book_start));

    // Pure applyDelta time
    metrics_.recordApplyLatency(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            book_end - book_start));

    metrics_.incDeltaApplied();
}
// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
std::string Reconciler::buildWebSocketTarget(const std::string &symbol) const
{
    std::string lower = boost::algorithm::to_lower_copy(symbol);
    // 100ms diff-depth stream (plain "@depth" batches updates every 1000ms).
    return "/ws/" + lower + "@depth@100ms";
}

// Validator thread. Posts a request and waits for the engine to fill it.
bool Reconciler::requestFreeze(FrozenBook &out, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(freeze_mutex_);
    freeze_target_ = &out;
    freeze_done_ = false;
    freeze_pending_.store(true, std::memory_order_release);

    const bool done = freeze_cv_.wait_for(lock, timeout, [this] {
        return freeze_done_ || !running_.load(std::memory_order_acquire);
    }) && freeze_done_;

    // Withdraw the request so a late engine never writes into `out`.
    freeze_target_ = nullptr;
    freeze_pending_.store(false, std::memory_order_release);
    return done;
}

// Engine thread, between messages. The book is not changing while we copy,
// because this thread is the only writer.
void Reconciler::serviceFreezeRequest()
{
    {
        std::lock_guard<std::mutex> lock(freeze_mutex_);
        if (freeze_target_ != nullptr && !freeze_done_)
        {
            const auto t0 = std::chrono::steady_clock::now();
            freeze_target_->levels = book_.snapshotActiveLevelsLocked();
            freeze_target_->seq = lastAppliedSeq_.load(std::memory_order_relaxed);
            freeze_target_->known_bid_floor = known_bid_floor_;
            freeze_target_->known_ask_ceiling = known_ask_ceiling_;
            freeze_target_->copy_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - t0).count();
            freeze_done_ = true;
        }
        freeze_pending_.store(false, std::memory_order_release);
    }
    freeze_cv_.notify_all();
}

void Reconciler::triggerResync()
{
    std::cout<<"triggerResync\n"; 
    resync_requested_ = true;
}

bool Reconciler::waitUntilInitialized(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(init_mutex_);
    auto pred = [this]
    {
        return initialized_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
    };
    bool ok;
    if (timeout == std::chrono::milliseconds::max())
    {
        init_cv_.wait(lock, pred);
        ok = true;
    }
    else
    {
        ok = init_cv_.wait_for(lock, timeout, pred);
    }
    return ok && initialized_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire);
}

void Reconciler::stop()
{
    std::cout << "[Reconciler] Shutdown requested." << std::endl;
    running_.store(false, std::memory_order_release);
    init_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(freeze_mutex_);  // wake a waiting requestFreeze()
    }
    freeze_cv_.notify_all();
    queue_.close();
}