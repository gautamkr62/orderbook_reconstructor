#include "engine/reconciler.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/delta.hpp"
#include "utils/price_parser.hpp"
#include "utils/metrics.hpp"
#include "engine/apply_delta.hpp"

#include <unordered_map>

#include <boost/algorithm/string.hpp>

#include <simdjson.h>

#include <chrono>
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

// ---------------------------------------------------------------------
// Helper: Parse raw JSON into a Delta struct
// ---------------------------------------------------------------------
namespace
{

    Delta parseDelta(const std::string &raw_msg, int tick_decimals, int qty_decimals)
    {
        thread_local simdjson::ondemand::parser parser;

        simdjson::padded_string padded(raw_msg);
        auto doc_result = parser.iterate(padded);
        if (doc_result.error())
        {
            throw std::runtime_error("parseDelta: simdjson parse failed");
        }
        auto doc = std::move(doc_result).value();

        Delta delta;
        delta.U = doc["U"];
        delta.u = doc["u"];

        auto bids = doc["b"].get_array().value();
        for (auto bid : bids)
        {
            auto price_qty = bid.get_array().value();
            std::string_view price_str = price_qty.at(0).get_string().value();
            std::string_view qty_str = price_qty.at(1).get_string().value();
            int64_t tick = parsePriceToTick(price_str, tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty_str, qty_decimals);
            delta.bids.emplace_back(tick, qty_scaled);
        }

        auto asks = doc["a"].get_array().value();
        for (auto ask : asks)
        {
            auto price_qty = ask.get_array().value();
            std::string_view price_str = price_qty.at(0).get_string().value();
            std::string_view qty_str = price_qty.at(1).get_string().value();
            int64_t tick = parsePriceToTick(price_str, tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty_str, qty_decimals);
            delta.asks.emplace_back(tick, qty_scaled);
        }

        return delta;
    }

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

// ---------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------
Reconciler::Reconciler(OrderBook &book,
                       RestClient &rest,
                       WebSocketClient &ws,
                       ThreadSafeQueue<std::string> &queue,
                       DiffHistoryBuffer &history,
                       MetricsCollector &metrics,
                       size_t window_size)
    : book_(book), rest_(rest), ws_(ws), queue_(queue), history_(history), metrics_(metrics), window_size_(window_size) {}

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
    std::cout << "[SymbolInfo] " << symbol
              << " tick_decimals=" << symbol_info_.tick_decimals
              << " qty_decimals=" << symbol_info_.qty_decimals << std::endl;
}

// ---------------------------------------------------------------------
// Public run()
// ---------------------------------------------------------------------
void Reconciler::run(const std::string &symbol)
{
    // Fetch symbol metadata
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

    // Set WebSocket message callback
    ws_.setMessageCallback([this](const std::string &msg)
                           {
        if (!queue_.tryPush(msg)) {
            std::cerr << "WARNING: Queue full, dropping WebSocket message." << std::endl;
        } });

    // Bootstrap loop
    while (running_.load(std::memory_order_acquire))
    {
        if (bootstrap(symbol_info_.symbol))
        {
            break;
        }
        std::cerr << "Bootstrap failed. Retrying in 2s..." << std::endl;
        interruptibleSleep(std::chrono::seconds(2), running_);
    }

    if (!running_.load())
        return;

    std::cout << "Bootstrap complete. lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;

    // Signal initialized
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        initialized_.store(true, std::memory_order_release);
    }
    init_cv_.notify_all();

    // Steady-state loop
    while (running_.load(std::memory_order_acquire))
    {
        // -----------------------------------------------------------------
        // 1. CHECK FOR DROPPED MESSAGES
        //    If the queue dropped any messages since the last check, force a
        //    resync immediately. This closes the gap where a dropped message
        //    might not be caught by the natural U/u gap detection.
        // -----------------------------------------------------------------
        uint64_t dropped = queue_.droppedCount();
        if (dropped > 0)
        {
            std::cerr << "[Engine] Detected " << dropped
                      << " dropped WebSocket messages. Triggering resync." << std::endl;
            queue_.resetDroppedCount(); // reset the counter
            resync_requested_ = true;   // trigger resync
            // Continue to the top of the loop – the resync will be handled below.
            continue;
        }

        // -----------------------------------------------------------------
        // 2. CHECK FOR RE-SYNC REQUEST
        // -----------------------------------------------------------------
        if (resync_requested_.exchange(false))
        {
            std::cout << "Re-sync triggered. Restarting bootstrap..." << std::endl;
            while (running_.load())
            {
                if (bootstrap(symbol_info_.symbol))
                {
                    break;
                }
                std::cerr << "Re-sync bootstrap failed. Retrying..." << std::endl;
                interruptibleSleep(std::chrono::seconds(2), running_);
            }
            if (!running_.load())
                break;
            std::cout << "Re-sync complete. lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;
            continue;
        }

        // -----------------------------------------------------------------
        // 3. PROCESS THE NEXT MESSAGE
        // -----------------------------------------------------------------
        auto maybe_msg = queue_.pop();

        static int log_counter = 0;
        if (++log_counter % 100 == 0) {
            std::cout << "[ENGINE-STATS] Queue size=" << queue_.size()
                    << " lastAppliedSeq=" << lastAppliedSeq_.load()
                    << std::endl;
        }


        if (!maybe_msg.has_value())
        {
            std::cout << "[Engine] Queue closed. Exiting loop." << std::endl;
            break;
        }

        processLiveDelta(maybe_msg.value());
    }

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

    try
    {
        std::cout << "[Bootstrap] Starting for " << symbol << std::endl;

        // Mark not initialized
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
        RestResponse snapshot_res = rest_.fetchDepthSnapshot(symbol, 5000);
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
            auto price_qty = bid.get_array().value();
            std::string_view price = price_qty.at(0).get_string().value();
            std::string_view qty = price_qty.at(1).get_string().value();
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
            auto price_qty = ask.get_array().value();
            std::string_view price = price_qty.at(0).get_string().value();
            std::string_view qty = price_qty.at(1).get_string().value();
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

        // last_bootstrap_bids_ = parsed_bids;
        // last_bootstrap_asks_ = parsed_asks;

        // Reset and apply snapshot atomically
        {
            std::unique_lock lock(book_.getMutex());
            book_.reset(best_bid_tick, best_ask_tick, window_size_);
            book_.applySnapshot(parsed_bids, parsed_asks);
            lastAppliedSeq_.store(S, std::memory_order_release);
        }

        {
            auto snap = book_.snapshotActiveLevels();
            // verifyBootstrapSnapshot("A", snap);
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

        {
            auto snap = book_.snapshotActiveLevels();
            // verifyBootstrapSnapshot("B", snap);
        }

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

        Delta delta;
        try
        {
            delta = parseDelta(maybe_msg.value(), tick_decimals, qty_decimals);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[DRAIN] parseDelta failed: " << e.what() << std::endl;
            continue;
        }

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

        {
            std::unique_lock lock(book_.getMutex());
            applyDelta(delta, book_);
            history_.push(std::move(delta));
            seq = u;
            lastAppliedSeq_.store(seq, std::memory_order_release);
        }
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
void Reconciler::processLiveDelta(const std::string &raw_msg)
{
    const auto process_start = std::chrono::steady_clock::now();

    // std::cout << "In processLiveDelta function\n";

    Delta delta;
    try
    {
        delta = parseDelta(raw_msg,
                           symbol_info_.tick_decimals,
                           symbol_info_.qty_decimals);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[LIVE] parseDelta failed: " << e.what() << std::endl;
        return;
    }

    const uint64_t U = delta.U;
    const uint64_t u = delta.u;
    const uint64_t seq = lastAppliedSeq_.load(std::memory_order_acquire);

    // Stale / duplicate
    if (u <= seq)
    {
        // std::cout << "[LIVE][SEQ] U=" << U << " u=" << u
        //   << " seq=" << seq << " -> STALE" << std::endl;
        return;
    }

    // Strict contiguity in steady state
    if (U != seq + 1)
    {
        // const bool overlap = U <= seq;
        // std::cerr << "[LIVE][SEQ] U=" << U << " u=" << u
        //           << " seq=" << seq << " expected=" << seq + 1
        //           << " -> " << (overlap ? "OVERLAP" : "GAP")
        //           << " -> RESYNC" << std::endl;
        resync_requested_ = true;
        return;
    }

    // std::cout << "[LIVE][SEQ] U=" << U << " u=" << u
    //           << " seq=" << seq << " -> CONTIGUOUS" << std::endl;

    // Apply

    const auto critical_start = std::chrono::steady_clock::now();
    {
        std::unique_lock lock(book_.getMutex());
        applyDelta(delta, book_);
        history_.push(std::move(delta));
        lastAppliedSeq_.store(u, std::memory_order_release);
    }

    const auto critical_end = std::chrono::steady_clock::now();

    metrics_.recordCriticalSectionLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(critical_end - critical_start));
    metrics_.recordApplyLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - process_start));
    metrics_.incDeltaApplied();
}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
std::string Reconciler::buildWebSocketTarget(const std::string &symbol) const
{
    std::string lower = boost::algorithm::to_lower_copy(symbol);
    return "/ws/" + lower + "@depth";
}

void Reconciler::triggerResync()
{
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
    queue_.close();
}