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
#include <cctype>      // for std::isdigit

// ---------------------------------------------------------------------
// RAII helper: ScopedSync
// ---------------------------------------------------------------------
class ScopedSync {
public:
    explicit ScopedSync(std::atomic<bool>& flag) : flag_(flag) {
        flag_.store(true, std::memory_order_release);
    }

    ~ScopedSync() {
        flag_.store(false, std::memory_order_release);
    }

    ScopedSync(const ScopedSync&) = delete;
    ScopedSync& operator=(const ScopedSync&) = delete;

private:
    std::atomic<bool>& flag_;
};

// ---------------------------------------------------------------------
// Helper: interruptible sleep for retries
// ---------------------------------------------------------------------
static void interruptibleSleep(std::chrono::milliseconds duration,
                               const std::atomic<bool>& running) {
    constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
    auto elapsed = std::chrono::milliseconds(0);
    while (elapsed < duration && running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(POLL_INTERVAL);
        elapsed += POLL_INTERVAL;
    }
}






// ---------------------------------------------------------------------
// Helper: Parse raw JSON into a Delta struct (with thread‑local parser)
// ---------------------------------------------------------------------
namespace {

Delta parseDelta(const std::string& raw_msg, int tick_decimals, int qty_decimals) {
    thread_local simdjson::ondemand::parser parser;

    simdjson::padded_string padded(raw_msg);
    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) {
        throw std::runtime_error("parseDelta: simdjson parse failed");
    }
    auto doc = std::move(doc_result).value();

    Delta delta;
    delta.U = doc["U"];
    delta.u = doc["u"];

    auto bids = doc["b"].get_array().value();
    for (auto bid : bids) {
        auto price_qty = bid.get_array().value();
        std::string_view price_str = price_qty.at(0).get_string().value();
        std::string_view qty_str = price_qty.at(1).get_string().value();
        int64_t tick = parsePriceToTick(price_str, tick_decimals);
        uint64_t qty_scaled = parseQuantityToScaled(qty_str, qty_decimals);
        delta.bids.emplace_back(tick, qty_scaled);
    }

    auto asks = doc["a"].get_array().value();
    for (auto ask : asks) {
        auto price_qty = ask.get_array().value();
        std::string_view price_str = price_qty.at(0).get_string().value();
        std::string_view qty_str = price_qty.at(1).get_string().value();
        int64_t tick = parsePriceToTick(price_str, tick_decimals);
        uint64_t qty_scaled = parseQuantityToScaled(qty_str, qty_decimals);
        delta.asks.emplace_back(tick, qty_scaled);
    }

    return delta;
}

} // unnamed namespace

// ---------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------
Reconciler::Reconciler(OrderBook& book,
                       RestClient& rest,
                       WebSocketClient& ws,
                       ThreadSafeQueue<std::string>& queue,
                       DiffHistoryBuffer& history,
                       MetricsCollector& metrics,
                       size_t window_size)
    : book_(book)
    , rest_(rest)
    , ws_(ws)
    , queue_(queue)
    , history_(history)
    , metrics_(metrics)
    , window_size_(window_size) {}

// ---------------------------------------------------------------------
// fetchSymbolInfo – Fills the single source of truth.
// ---------------------------------------------------------------------

static int countSignificantDecimals(std::string_view str) {
    size_t dot = str.find('.');
    if (dot == std::string_view::npos) return 0;
    // Count digits from the end, skipping trailing zeros.
    size_t end = str.size() - 1;
    while (end > dot && str[end] == '0') --end;
    if (end == dot) return 0; // no significant digits
    return static_cast<int>(end - dot);
}


void Reconciler::fetchSymbolInfo(const std::string& symbol) {
    // 1. Call the exchangeInfo endpoint.
    RestResponse response = rest_.fetchExchangeInfo(symbol);
    if (!response.ok()) {
        throw std::runtime_error("fetchSymbolInfo: failed to fetch exchangeInfo for " + symbol);
    }

    // 2. Parse JSON.
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(response.body);
    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) {
        throw std::runtime_error("fetchSymbolInfo: failed to parse exchangeInfo JSON");
    }
    auto doc = std::move(doc_result).value();

    // Navigate to the symbol info.
    auto symbols = doc["symbols"].get_array();
    bool found = false;
    for (auto sym : symbols) {
        std::string_view sym_name = sym["symbol"].get_string();
        if (sym_name != symbol) continue;

        found = true;
        auto filters = sym["filters"].get_array();
        for (auto filter : filters) {
            std::string_view filter_type = filter["filterType"].get_string();
            if (filter_type == "PRICE_FILTER") {
                std::string_view tick_size = filter["tickSize"].get_string();
                symbol_info_.tick_decimals = countSignificantDecimals(tick_size);
            } 
            else if (filter_type == "LOT_SIZE") {
                std::string_view step_size = filter["stepSize"].get_string();
                symbol_info_.qty_decimals = countSignificantDecimals(step_size);
            }
        }
        break;
    }

    if (!found) {
        throw std::runtime_error("fetchSymbolInfo: symbol " + symbol + " not found in exchangeInfo");
    }

    // Store symbol name.
    symbol_info_.symbol = symbol;

    std::cout << "[SymbolInfo] " << symbol
              << " tick_decimals=" << symbol_info_.tick_decimals
              << " qty_decimals=" << symbol_info_.qty_decimals << std::endl;
}
// ---------------------------------------------------------------------
// Public run()
// ---------------------------------------------------------------------
void Reconciler::run(const std::string& symbol) {
    fetchSymbolInfo(symbol);

    ws_.setMessageCallback([this](const std::string& msg) {
        if (!queue_.tryPush(msg)) {
            std::cerr << "WARNING: Queue full, dropping WebSocket message." << std::endl;
        }
    });

    while (running_.load()) {
        try {
            fetchSymbolInfo(symbol);
            break;
        } catch (const std::exception& e) {
            std::cerr << "[Reconciler] Failed to fetch symbol info: " << e.what()
                      << ". Retrying in 2s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
    if (!running_.load()) return;


    // --- Bootstrap (retry until success or shutdown) ---
    while (running_.load(std::memory_order_acquire)) {
        if (bootstrap(symbol_info_.symbol)) {
            break;
        }
        std::cerr << "Bootstrap failed. Retrying in 2s..." << std::endl;
        interruptibleSleep(std::chrono::seconds(2), running_);
    }

    if (!running_.load()) {
        std::cout << "Shutdown during bootstrap. Exiting." << std::endl;
        return;
    }

    std::cout << "Bootstrap complete. lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;

    // ---- SIGNAL INITIALIZATION ----
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        initialized_.store(true, std::memory_order_release);
    }
    init_cv_.notify_all();

    // ---- Steady-State Loop ----
    while (running_.load(std::memory_order_acquire)) {
        if (resync_requested_.exchange(false)) {
            std::cout << "Re-sync triggered. Restarting bootstrap..." << std::endl;
            // Note: We intentionally do not set initialized_=false here because
            // the Validator is already running and we want it to keep reading.
            // The book is rebuilt under lock, so consistency is maintained.
            while (running_.load()) {
                if (bootstrap(symbol_info_.symbol)) {
                    break;
                }
                std::cerr << "Re-sync bootstrap failed. Retrying..." << std::endl;
                interruptibleSleep(std::chrono::seconds(2), running_);
            }
            if (!running_.load()) break;
            std::cout << "Re-sync complete. lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;
            continue;
        }

        auto maybe_msg = queue_.pop();
        if (!maybe_msg.has_value()) {
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
bool Reconciler::bootstrap(const std::string& symbol) {
    ScopedSync syncing(syncing_);

    try {
        std::cout << "[Bootstrap] Starting for " << symbol << std::endl;

        // ---- Step 0: Mark as not initialized ----
        {
            std::lock_guard<std::mutex> lock(init_mutex_);
            initialized_.store(false, std::memory_order_release);
        }

        // ---- Step 1: Close any existing WebSocket ----
        if (ws_.isConnected() || ws_.isConnecting()) {
            std::cout << "[Bootstrap] Closing existing WebSocket..." << std::endl;
            ws_.disconnect();
            auto res = ws_.waitForClose(std::chrono::seconds(5));
            if (res == CloseResult::Timeout) {
                ws_.forceClose();
                ws_.waitForClose(std::chrono::seconds(2));
            }
            if (!running_.load()) {
                std::cout << "[Bootstrap] Shutdown during close. Aborting." << std::endl;
                return false;
            }
        }

        // ---- Step 2: Connect WebSocket ----
        std::string target = buildWebSocketTarget(symbol);
        std::cout << "[Bootstrap] Connecting to " << target << std::endl;
        ws_.connectAsync(target);

        auto res = ws_.waitForConnected(std::chrono::seconds(5));
        if (res != CloseResult::Closed || !running_.load()) {
            if (!running_.load()) {
                std::cout << "[Bootstrap] Shutdown during connect." << std::endl;
            } else {
                std::cerr << "[Bootstrap] WebSocket connection timeout." << std::endl;
            }
            ws_.forceClose();
            return false;
        }
        std::cout << "[Bootstrap] WebSocket connected." << std::endl;

        // ---- Step 3: Fetch REST Snapshot ----
        // RestResponse snapshot_res = rest_.fetchDepthSnapshot(symbol);
        RestResponse snapshot_res = rest_.fetchDepthSnapshot(symbol, 5000);

        if (!snapshot_res.ok()) {
            std::cerr << "[Bootstrap] Snapshot fetch failed: " << snapshot_res.body << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        // ---- Step 4: Parse Snapshot ----
        thread_local simdjson::ondemand::parser parser;
        auto doc_result = parser.iterate(snapshot_res.body);
        if (doc_result.error()) {
            std::cerr << "[Bootstrap] Failed to parse snapshot JSON." << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }
        auto doc = std::move(doc_result).value();

        uint64_t S = doc["lastUpdateId"];

        std::vector<std::pair<int64_t, uint64_t>> parsed_bids;
        std::vector<std::pair<int64_t, uint64_t>> parsed_asks;

        // Bids
        auto bids_array = doc["bids"].get_array();
        parsed_bids.reserve(bids_array.count_elements());
        int64_t best_bid_tick = -1;
        for (auto bid : bids_array) {
            auto price_qty = bid.get_array().value();
            std::string_view price = price_qty.at(0).get_string().value();
            std::string_view qty = price_qty.at(1).get_string().value();
            int64_t tick = parsePriceToTick(price, symbol_info_.tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty, symbol_info_.qty_decimals);
            parsed_bids.emplace_back(tick, qty_scaled);
            if (best_bid_tick == -1) best_bid_tick = tick;
        }

        // Asks
        auto asks_array = doc["asks"].get_array();
        parsed_asks.reserve(asks_array.count_elements());
        int64_t best_ask_tick = -1;
        for (auto ask : asks_array) {
            auto price_qty = ask.get_array().value();
            std::string_view price = price_qty.at(0).get_string().value();
            std::string_view qty = price_qty.at(1).get_string().value();
            int64_t tick = parsePriceToTick(price, symbol_info_.tick_decimals);
            uint64_t qty_scaled = parseQuantityToScaled(qty, symbol_info_.qty_decimals);
            parsed_asks.emplace_back(tick, qty_scaled);
            if (best_ask_tick == -1) best_ask_tick = tick;
        }

        if (parsed_bids.empty() || parsed_asks.empty()) {
            std::cerr << "[Bootstrap] Snapshot has empty levels." << std::endl;
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        // ---- Step 5: Reset book and apply snapshot atomically ----
        {
            std::unique_lock lock(book_.getMutex());
            book_.reset(best_bid_tick, best_ask_tick, window_size_);
            book_.applySnapshot(parsed_bids, parsed_asks);
            lastAppliedSeq_.store(S, std::memory_order_release);
        }

        // ---- Step 6: Drain the pending buffer ----
        bool success = drainPendingBuffer(S, symbol_info_.tick_decimals, symbol_info_.qty_decimals);

        if (!success) {
            std::cerr << "[Bootstrap] Failed during buffer drain. Clearing queue." << std::endl;
            queue_.clear();
            ws_.disconnect();
            ws_.waitForClose(std::chrono::seconds(2));
            return false;
        }

        // ---- Step 7: Mark initialization complete ----
        {
            std::lock_guard<std::mutex> lock(init_mutex_);
            initialized_.store(true, std::memory_order_release);
        }
        init_cv_.notify_all();

        std::cout << "[Bootstrap] Success. final lastAppliedSeq=" << lastAppliedSeq_.load() << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[Bootstrap] Exception: " << e.what() << std::endl;
        ws_.disconnect();
        ws_.waitForClose(std::chrono::seconds(2));
        return false;
    } catch (...) {
        std::cerr << "[Bootstrap] Unknown exception." << std::endl;
        ws_.disconnect();
        ws_.waitForClose(std::chrono::seconds(2));
        return false;
    }
}

bool Reconciler::drainPendingBuffer(uint64_t snapshot_id,
                                    int tick_decimals,
                                    int qty_decimals) {
    uint64_t S = snapshot_id;
    uint64_t seq = S;
    bool first_event_applied = false;

    while (true) {
        auto maybe_msg = queue_.tryPop();
        if (!maybe_msg.has_value()) break;

        Delta delta;
        try {
            delta = parseDelta(maybe_msg.value(), tick_decimals, qty_decimals);
        } catch (const std::exception& e) {
            std::cerr << "drainPendingBuffer: parseDelta failed: " << e.what() << std::endl;
            continue;
        }

        uint64_t U = delta.U;
        uint64_t u = delta.u;

        // Discard events older than or equal to the snapshot.
        if (u <= S) continue;

        // Find the first chaining event.
        if (!first_event_applied) {
            if (U <= S + 1 && u >= S + 1) {

                auto start = std::chrono::steady_clock::now();

                {
                    std::unique_lock lock(book_.getMutex());

                    applyDelta(delta, book_);
                    seq = u;
                    first_event_applied = true;
                    history_.push(std::move(delta));
                    lastAppliedSeq_.store(seq, std::memory_order_release);
                }

                auto end = std::chrono::steady_clock::now();

                metrics_.recordCriticalSectionLatency(
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                );
            } else if (U > S + 1) {
                std::cerr << "Gap before first event! U=" << U << " S=" << S << std::endl;
                return false;
            }
            continue;
        }

        // Steady-state gap check.
        if (U > seq + 1) {
            std::cerr << "Gap during drain! U=" << U << " seq=" << seq << std::endl;
            return false;
        }

        // Apply the delta (guaranteed U <= seq + 1).

        auto start = std::chrono::steady_clock::now();
        {
            std::unique_lock lock(book_.getMutex());
            applyDelta(delta, book_);
            seq = u;
            history_.push(std::move(delta));
            lastAppliedSeq_.store(seq, std::memory_order_release);
        }

        auto end = std::chrono::steady_clock::now();

        metrics_.recordCriticalSectionLatency(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start)
        );
    }

    return true;
}
// ---------------------------------------------------------------------
// Steady-State Processing
// ---------------------------------------------------------------------
void Reconciler::processLiveDelta(const std::string& raw_msg) {

    auto start = std::chrono::steady_clock::now();

    Delta delta;
    try {
        delta = parseDelta(raw_msg, symbol_info_.tick_decimals, symbol_info_.qty_decimals);
    } catch (const std::exception& e) {
        std::cerr << "parseDelta failed: " << e.what() << std::endl;
        return;
    }

    uint64_t U = delta.U;
    uint64_t u = delta.u;
    uint64_t seq = lastAppliedSeq_.load(std::memory_order_acquire);
    // std::cout << "[Engine] Pushed delta U=" << U << " u=" << u << std::endl;

    if (u <= seq) return;

    if (U > seq + 1) {
        std::cerr << "LIVE GAP! U=" << U << " seq=" << seq << std::endl;
        resync_requested_ = true;
        return;
    }

    auto start = std::chrono::steady_clock::now();




    {
        std::unique_lock lock(book_.getMutex());

        lastAppliedSeq_.store(u, std::memory_order_release);
        applyDelta(delta, book_);
        history_.push(std::move(delta));
    }

    auto end = std::chrono::steady_clock::now();

    metrics_.recordCriticalSectionLatency(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start)
    );

    auto end = std::chrono::steady_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    metrics_.recordApplyLatency(latency);
    metrics_.incDeltaApplied();

}

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
std::string Reconciler::buildWebSocketTarget(const std::string& symbol) const {
    std::string lower = boost::algorithm::to_lower_copy(symbol);
    return "/ws/" + lower + "@depth";
}

void Reconciler::triggerResync() {
    resync_requested_ = true;
}

bool Reconciler::waitUntilInitialized(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(init_mutex_);
    auto pred = [this] {
        return initialized_.load(std::memory_order_acquire) ||
               !running_.load(std::memory_order_acquire);
    };
    bool ok;
    if (timeout == std::chrono::milliseconds::max()) {
        init_cv_.wait(lock, pred);
        ok = true;
    } else {
        ok = init_cv_.wait_for(lock, timeout, pred);
    }
    // Return true only if we are initialized AND still running.
    return ok && initialized_.load(std::memory_order_acquire) &&
           running_.load(std::memory_order_acquire);
}

void Reconciler::stop() {
    std::cout << "[Reconciler] Shutdown requested." << std::endl;
    running_.store(false, std::memory_order_release);
    init_cv_.notify_all();
    queue_.close();
}