#include "engine/reconciler.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/delta.hpp"
#include "utils/price_parser.hpp"
#include "utils/metrics.hpp"
#include "engine/apply_delta.hpp"


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


    
    // Ask everystep -> Does this work actually need to happen?

// thread_local std::string parse_buffer;
// thread_local simdjson::ondemand::parser parser;


namespace {
    // In your header / top of reconciler.cpp — thread_local storage
    thread_local std::string        tl_raw_buf;   // retained capacity, replaces padded_string
    thread_local simdjson::ondemand::parser tl_parser;

    

// ---------------------------------------------------------------
// Fast integer-only price/qty parser — no strtod, no FP, no div
// ---------------------------------------------------------------
    static inline uint64_t parseScaledFast(const char* p, int len, int decimals)
    {
        uint64_t result  = 0;
        int      dot_pos = len; // assume no decimal point

        for (int i = 0; i < len; ++i) {
            if (p[i] == '.') { dot_pos = i; continue; }
            result = result * 10 + static_cast<uint64_t>(p[i] - '0');
        }

        // actual decimal digits present in the string
        const int actual_dec = (dot_pos == len) ? 0 : (len - dot_pos - 1);
        const int diff       = decimals - actual_dec;

        // scale up (common) or truncate excess precision (rare)
        // POW10 table — avoids any multiply loop
        static constexpr uint64_t POW10[] = {
            1ULL, 10ULL, 100ULL, 1'000ULL, 10'000ULL,
            100'000ULL, 1'000'000ULL, 10'000'000ULL, 100'000'000ULL
        };

        if (diff > 0) result *= POW10[diff];
        if (diff < 0) result /= POW10[-diff]; // only when exchange sends extra dp

        return result;
    }

// ---------------------------------------------------------------
// parseDelta
//   - no Delta struct built first then applied separately
//   - no padded_string realloc on every call
//   - no at(0)/at(1) rescan
//   - no in_dec flag per character
//   - sequence numbers extracted, then early-exit before side parsing
// ---------------------------------------------------------------
    void parseDelta(const std::string& raw_msg,
                    int                tick_decimals,
                    int                qty_decimals,
                    Delta&             delta)
    {
        // ------------------------------------------------------------------
        // 1. Copy into retained buffer — only allocates when message grows
        //    beyond seen maximum. Zero-cost on the common path.
        // ------------------------------------------------------------------
        const size_t needed = raw_msg.size() + simdjson::SIMDJSON_PADDING;
        if (tl_raw_buf.size() < needed)
            tl_raw_buf.resize(needed * 2); // grow with headroom, amortised

        std::memcpy(tl_raw_buf.data(), raw_msg.data(), raw_msg.size());
        // simdjson requires padding bytes to be zeroed
        std::memset(tl_raw_buf.data() + raw_msg.size(), 0, simdjson::SIMDJSON_PADDING);

        simdjson::padded_string_view psv(
            tl_raw_buf.data(),
            raw_msg.size(),
            tl_raw_buf.size()   // capacity >= size + PADDING
        );

        // ------------------------------------------------------------------
        // 2. Parse document — simdjson ondemand, no DOM allocation
        // ------------------------------------------------------------------


        // const auto t1 = std::chrono::steady_clock::now();
        auto doc = tl_parser.iterate(psv);
        // const auto t2 = std::chrono::steady_clock::now();


        delta.bids.clear(); // retains vector capacity across calls
        delta.asks.clear();

        // ------------------------------------------------------------------
        // 3. Sequence numbers first — caller can early-exit before sides
        // ------------------------------------------------------------------
        if (doc.find_field("U").get(delta.U))
            throw std::runtime_error("parseDelta: missing U");
        if (doc.find_field("u").get(delta.u))
            throw std::runtime_error("parseDelta: missing u");

        // ------------------------------------------------------------------
        // 4. Parse each side with iterator — no at(0)/at(1) rescan
        //    Apply inline: no second loop in applyDelta needed
        // ------------------------------------------------------------------
        auto parse_side = [&](const char* key,
                            std::vector<LevelUpdate>& levels)
        {
            auto arr_result = doc.find_field(key).get_array();
            if (arr_result.error()) return; // missing side is valid (empty)

            for (auto entry : arr_result.value_unsafe()) {
                auto pq = entry.get_array();
                if (pq.error()) continue;

                auto it = pq.value_unsafe().begin();

                std::string_view price_sv, qty_sv;
                (*it).get_string().get(price_sv); ++it;
                (*it).get_string().get(qty_sv);

                const int64_t  tick = static_cast<int64_t>(
                    parseScaledFast(price_sv.data(),
                                    static_cast<int>(price_sv.size()),
                                    tick_decimals));

                const uint64_t qty  = parseScaledFast(
                    qty_sv.data(),
                    static_cast<int>(qty_sv.size()),
                    qty_decimals);

                levels.emplace_back(tick, qty);
            }
        };

        parse_side("b", delta.bids);
        parse_side("a", delta.asks);

    }

    void prefaultThreadLocals(int tick_decimals, int qty_decimals) {
        // Pre-size the buffer — no allocation on first real message
        tl_raw_buf.resize(8192 + simdjson::SIMDJSON_PADDING);
        std::memset(tl_raw_buf.data(), 0, tl_raw_buf.size());

        // Warm simdjson's internal allocations too
        Delta tmp;
        tmp.bids.reserve(2048);
        tmp.asks.reserve(2048);
        std::string dummy(R"({"U":1,"u":2,"b":[["0.01","1.0"]],"a":[["0.02","1.0"]]})");
        parseDelta(dummy, tick_decimals, qty_decimals, tmp);
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


void prefaultEngineThreadLocals() {
    prefaultThreadLocals(2, 5);
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
    // 0. PRE-FAULT — just the buffer, before fetchSymbolInfo
    // -----------------------------------------------------------------
    tl_raw_buf.resize(8192 + simdjson::SIMDJSON_PADDING);
    std::memset(tl_raw_buf.data(), 0, tl_raw_buf.size());

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
    prefaultThreadLocals(symbol_info_.tick_decimals, symbol_info_.qty_decimals);



    // -----------------------------------------------------------------
    // 2. SET WEBSOCKET CALLBACK (I/O THREAD)
    // -----------------------------------------------------------------
    ws_.setMessageCallback(
        [this](const std::string &msg)
        {
            const auto wire_time = std::chrono::steady_clock::now();
            if (!queue_.tryPush(QueuedDelta{msg, wire_time}))
            {
                std::cerr << "WARNING: Queue full, dropping WebSocket message." << std::endl;
            }
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
        while (running_.load(std::memory_order_acquire))
        {
            while (queue_.tryPop(msg))
            {
                processLiveDelta(msg.raw_msg, msg.wire_time);

                if (!running_.load(std::memory_order_relaxed))
                    break;

                if (resync_requested_.load(std::memory_order_acquire) ||
                    queue_.droppedCount() > 0)
                    break;
            }

            if (resync_requested_.load(std::memory_order_acquire) ||
                queue_.droppedCount() > 0)
                break;

            _mm_pause();  // ← only change
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

        {
            std::unique_lock lock(book_.getMutex());

            // Preserve any growth from the previous window (avoid resetting to
            // the default size after a resync).
            size_t current_size = book_.currentWindowSize();
            size_t new_window_size = std::max(window_size_, current_size);

            book_.reset(best_bid_tick, best_ask_tick, new_window_size);
            book_.applySnapshot(parsed_bids, parsed_asks);
            lastAppliedSeq_.store(S, std::memory_order_release);
        }

        {
            // auto snap = book_.snapshotActiveLevels();
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

        const std::string& raw = maybe_msg->raw_msg;
        // const auto wire_time = maybe_msg->wire_time;

        thread_local Delta delta;

        if (delta.bids.capacity() < 2048)
            delta.bids.reserve(2048);

        if (delta.asks.capacity() < 2048)
            delta.asks.reserve(2048);

        parseDelta(
            raw,
            tick_decimals,
            qty_decimals,
            delta
        );

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
            seq = u;
            lastAppliedSeq_.store(seq, std::memory_order_release);
        }
        history_.push(std::move(delta));
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


void Reconciler::processLiveDelta(const std::string& raw_msg,
                                   std::chrono::steady_clock::time_point wire_time)
{
    // ---------------------------
    // 1. Parse
    // ---------------------------
    const auto parse_start = std::chrono::steady_clock::now();
    thread_local Delta delta;
    if (delta.bids.capacity() < 2048) delta.bids.reserve(2048);
    if (delta.asks.capacity() < 2048) delta.asks.reserve(2048);
    const auto create_delta = std::chrono::steady_clock::now(); 
    // std::cout<<"Creating delta - >"<<create_delta - parse_start<<"\nTime to reach processLiveDelta from set messagecallback -> "<< parse_start - wire_time<<"\n"; 

    // Reused per-thread Delta with retained vector capacity across calls.

    
    parseDelta(
        raw_msg,
        symbol_info_.tick_decimals,
        symbol_info_.qty_decimals,
        delta
    );


    const auto parse_end = std::chrono::steady_clock::now();
    // std::cout<<"parse_end - parse start time -> "<<parse_end - parse_start<< "\n"; 
    metrics_.recordParseLatency(
    std::chrono::duration_cast<std::chrono::microseconds>(
        parse_end - parse_start));

    // Capture sizes now — history_.push will move-empty these vectors.
    // const size_t bid_count = delta.bids.size();
    // const size_t ask_count = delta.asks.size();

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
    // 3. Critical section
    // ---------------------------
    const auto lock_start = std::chrono::steady_clock::now();

    std::unique_lock lock(book_.getMutex());

    // const auto lock_acquired = std::chrono::steady_clock::now();

    const auto book_start = std::chrono::steady_clock::now();
    applyDelta(delta, book_);
    const auto book_end = std::chrono::steady_clock::now();

    lastAppliedSeq_.store(u, std::memory_order_release);

    const auto critical_end = std::chrono::steady_clock::now();

    lock.unlock();

    // ---------------------------
    // 4. History & metrics
    // ---------------------------
    history_.push(delta);   // copy — keeps this thread_local Delta's capacity intact for reuse
    // If history_ can be changed to accept a lightweight snapshot/view instead of
    // owning a full Delta copy, that would remove this copy's cost entirely.

    // End-to-end: wire → book updated (excludes history push)
    metrics_.recordE2ELatency(
        std::chrono::duration_cast<std::chrono::microseconds>(
            book_end - wire_time));

    // Critical section: lock wait + apply + seq store
    metrics_.recordCriticalSectionLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(
            critical_end - lock_start));

    // Pure applyDelta time
    metrics_.recordApplyLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(
            book_end - book_start));

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
    queue_.close();
}