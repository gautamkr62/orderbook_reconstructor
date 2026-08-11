#include "engine/validator.hpp"

#include "utils/price_parser.hpp"
#include "engine/delta.hpp"
#include "utils/metrics.hpp"


#include <simdjson.h>

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <iomanip>

void applyDelta(const Delta &delta, OrderBook &book);

// ---------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------
Validator::Validator(OrderBook &live_book,
                     DiffHistoryBuffer &history,
                     RestClient &rest,
                     Reconciler &reconciler,
                     MetricsCollector& metrics,
                     size_t window_size,
                     std::chrono::seconds interval)
    : live_book_(live_book), history_(history), rest_(rest), reconciler_(reconciler) , metrics_(metrics), window_size_(window_size), interval_(interval) {}

Validator::~Validator()
{
    stop();
}

void Validator::stop()
{
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
}

void Validator::updateTimestamp()
{
    last_validation_timestamp_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count(),
        std::memory_order_release);
}

// ---------------------------------------------------------------------
// run() – Main loop
// ---------------------------------------------------------------------
void Validator::run()
{
    if (!reconciler_.waitUntilInitialized(std::chrono::seconds(30)))
    {
        std::cerr << "[Validator] Reconciler not initialized. Exiting." << std::endl;
        return;
    }

    std::cout << "[Validator] Thread started in validator.\n";

    while (running_.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(cv_mutex_);
        bool stopped = cv_.wait_for(
            lock,
            interval_,
            [this]
            {
                return !running_.load(std::memory_order_acquire);
            });
        if (stopped)
            break;
        lock.unlock();

        if (reconciler_.isSyncing())
        {
            continue;
        }

        performValidation();
    }

    std::cout << "[Validator] Thread stopped.\n";
}

// ---------------------------------------------------------------------
// performValidation() – Orchestrates validation with retry logic
// ---------------------------------------------------------------------
bool Validator::performValidation()
{
    if (reconciler_.isSyncing())
    {
        metrics_.incValidationMismatch();
        return false;
    }

    std::cout << "[Validator] Starting validation cycle..." << std::endl;

    auto runAttempt =
        [&](std::string &mismatch_details,
            bool &snapshot_ahead,
            bool &validation_completed) -> bool
    {
        validation_completed = false;

        // ---- Build shadow ----
        ShadowResult shadow_result;
        if (!buildShadowBook(shadow_result))
        {
            updateTimestamp();
            metrics_.incValidationMismatch();
            return false;
        }

        // ---- Freeze live book ----
        uint64_t F = 0;
        std::vector<std::pair<int64_t, uint64_t>> frozen_bids;
        std::vector<std::pair<int64_t, uint64_t>> frozen_asks;

        if (!freeze(F, frozen_bids, frozen_asks))
        {
            updateTimestamp();
            metrics_.incValidationMismatch();
            return false;
        }

        // ---- Snapshot ahead of freeze? Not a failure, just an abort. ----
        if (shadow_result.snapshot_seq > F)
        {
            size_t gap = shadow_result.snapshot_seq - F;
            if (gap < 2000)
            {
                std::cout << "[Validator] Snapshot ahead (gap=" << gap
                          << "). Waiting for live book to catch up..." << std::endl;

                // Wait up to 5 seconds for the live book to catch up.
                auto start = std::chrono::steady_clock::now();
                const auto timeout = std::chrono::seconds(5);
                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    uint64_t current_F = reconciler_.getLastAppliedSeq();
                    if (current_F >= shadow_result.snapshot_seq)
                    {
                        break;
                    }
                    if (std::chrono::steady_clock::now() - start > timeout)
                    {
                        std::cout << "[Validator] Timeout waiting for live book to catch up." << std::endl;
                        break;
                    }
                }

                // Re‑freeze after waiting.
                if (!freeze(F, frozen_bids, frozen_asks))
                {
                    updateTimestamp();
                    metrics_.incValidationMismatch();
                    return false;
                }
                // Now proceed to replay and compare (continue execution).
                // Note: We must NOT `return true` here – we need to continue validation.
            }
            else
            {
                // Large gap – skip the round (rare).
                std::cout << "[Validator] Large gap detected: " << gap << ". Skipping this round." << std::endl;
                updateTimestamp();
                metrics_.incValidationSkipped();
                return true;
            }
        }
        snapshot_ahead = false;

        // ---- Replay ----
        if (!replayHistory(shadow_result.snapshot_seq, F, *shadow_result.book))
        {
            updateTimestamp();
            metrics_.incValidationMismatch();
            return false;
        }

        // From here on, validation genuinely ran -- a false return below
        // means a REAL mismatch, not an abort.
        validation_completed = true;

        // ---- Drift telemetry ----
        // Counts ATTEMPTS, not logical validation cycles -- a retried
        // cycle contributes 2 to these totals. Named accordingly so a
        // future success-rate calculation isn't misleading.
        ++validation_attempts_;

        auto shadow_snapshot = shadow_result.book->snapshotActiveLevels();

        size_t bid_tail_drift =
            shadow_result.bid_depth > shadow_snapshot.bids.size()
                ? shadow_result.bid_depth - shadow_snapshot.bids.size()
                : 0;
        size_t ask_tail_drift =
            shadow_result.ask_depth > shadow_snapshot.asks.size()
                ? shadow_result.ask_depth - shadow_snapshot.asks.size()
                : 0;

        if (bid_tail_drift > 0)
            ++bid_tail_drift_attempts_;
        if (ask_tail_drift > 0)
            ++ask_tail_drift_attempts_;

        max_bid_tail_drift_ = std::max(max_bid_tail_drift_, bid_tail_drift);
        max_ask_tail_drift_ = std::max(max_ask_tail_drift_, ask_tail_drift);

        // ---- Compare ----
        bool passed = compareBooks(*shadow_result.book, frozen_bids, frozen_asks,
                                   shadow_result.bid_depth, shadow_result.ask_depth,
                                   mismatch_details);

        updateTimestamp();

        if (passed)
        {
            success_count_.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[Validator] Validation PASSED "
                      << "(tail drift: bid=" << bid_tail_drift
                      << ", ask=" << ask_tail_drift << ")" << std::endl;
            metrics_.incValidationSuccess();

        }
        else
        {
            live_book_.dumpActiveLevels();
            shadow_result.book->dumpActiveLevels();
            metrics_.incValidationMismatch();
 
        }

        return passed;
    };

    // ---- Attempt 1 ----
    std::string mismatch_details_1;
    bool snapshot_ahead_1 = false;
    bool completed_1 = false;

    if (runAttempt(mismatch_details_1, snapshot_ahead_1, completed_1))
    {
        return true;
    }
    // A false return with completed_1 == true means attempt 1 found a
    // REAL mismatch -- remember that regardless of what attempt 2 does.
    const bool attempt1_genuine_failure = completed_1;

    // ---- Retry policy ----
    if (snapshot_ahead_1)
    {
        std::cout << "[Validator] Snapshot ahead. Retrying immediately." << std::endl;
    }
    else
    {
        std::cout << "[Validator] Validation failed. Retrying in 5 seconds." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    // ---- Attempt 2 ----
    std::string mismatch_details_2;
    bool snapshot_ahead_2 = false;
    bool completed_2 = false;

    if (runAttempt(mismatch_details_2, snapshot_ahead_2, completed_2))
    {
        // Attempt 2 passed -- but if attempt 1 found a genuine mismatch,
        // that's still real evidence of a transient inconsistency that
        // self-corrected. Log it, but don't treat the cycle as a clean
        // pass-through; still don't resync since the book is provably
        // correct right now.
        if (attempt1_genuine_failure)
        {
            std::cout << "[Validator] Note: attempt 1 found a mismatch ("
                      << mismatch_details_1 << ") but attempt 2 passed. "
                      << "Not resyncing since current state is verified correct."
                      << std::endl;
        }
        std::cout << "[Validator] Retry PASSED." << std::endl;
        return true;
    }
    const bool attempt2_genuine_failure = completed_2;

    // ---- Neither attempt completed with a genuine failure => pure abort ----
    if (!attempt1_genuine_failure && !attempt2_genuine_failure)
    {
        std::cout << "[Validator] Validation skipped (no attempt completed "
                     "-- snapshot-ahead or transient failure). No resync."
                  << std::endl;
        return false;
    }

    // ---- At least one attempt found a genuine mismatch: real evidence. ----
    // Prefer the most recent genuine failure's details; fall back to the
    // earlier one if attempt 2 aborted rather than genuinely failing.
    const std::string &final_mismatch =
        attempt2_genuine_failure ? mismatch_details_2 : mismatch_details_1;

    mismatch_count_.fetch_add(1, std::memory_order_relaxed);
    resyncs_triggered_.fetch_add(1, std::memory_order_relaxed);

    std::cerr << "[Validator] Validation failed: " << final_mismatch << std::endl;

    reconciler_.triggerResync();
    return false;
}
// ---------------------------------------------------------------------
// freeze() – Capture consistent snapshot under shared_lock
// ---------------------------------------------------------------------
bool Validator::freeze(uint64_t &F,
                       std::vector<std::pair<int64_t, uint64_t>> &frozen_bids,
                       std::vector<std::pair<int64_t, uint64_t>> &frozen_asks)
{
    std::shared_lock lock(live_book_.getMutex());
    if (!live_book_.verifyConsistency())
    {
        std::cerr << "[Validator] Live book consistency check failed! Skipping this validation round." << std::endl;
        return false;
    }

    auto snap = live_book_.snapshotActiveLevels();
    F = reconciler_.getLastAppliedSeq();
    std::cout << "[Live] At sequence F=" << F
              << " bids=" << snap.bids.size()
              << " asks=" << snap.asks.size()
              << " best_bid=" << (snap.bids.empty() ? 0 : snap.bids[0].first)
              << " best_ask=" << (snap.asks.empty() ? 0 : snap.asks[0].first)
              << std::endl;

    frozen_bids = std::move(snap.bids);
    frozen_asks = std::move(snap.asks);

    if (frozen_bids.empty() || frozen_asks.empty())
    {
        std::cerr << "[Validator] Frozen book empty." << std::endl;
        return false;
    }
    metrics_.setActiveBids(frozen_bids.size()); 
    metrics_.setActiveAsks(frozen_asks.size()); 
    return true;
}

// ---------------------------------------------------------------------
// parseSnapshot() – Shared JSON → vector parsing
// ---------------------------------------------------------------------
bool Validator::parseSnapshot(const std::string &json,
                              uint64_t &snapshot_seq,
                              std::vector<std::pair<int64_t, uint64_t>> &parsed_bids,
                              std::vector<std::pair<int64_t, uint64_t>> &parsed_asks)
{
    std::cout << "[Validator] parseSnapshot: parsing JSON of size " << json.size() << " bytes" << std::endl;

    simdjson::ondemand::parser parser;
    const auto &info = reconciler_.symbolInfo();

    simdjson::padded_string padded(json);
    auto doc_result = parser.iterate(padded);
    if (doc_result.error())
    {
        std::cerr << "[Validator] simdjson parse error: " << simdjson::error_message(doc_result.error()) << std::endl;
        std::cerr << "[Validator] JSON snippet: " << json.substr(0, 200) << "..." << std::endl;
        return false;
    }
    auto doc = std::move(doc_result).value();

    snapshot_seq = doc["lastUpdateId"];

    // Bids
    auto bids_arr = doc["bids"].get_array();
    parsed_bids.reserve(bids_arr.count_elements());
    for (auto bid : bids_arr)
    {
        auto price_qty = bid.get_array().value();
        std::string_view price_str = price_qty.at(0).get_string().value();
        std::string_view qty_str = price_qty.at(1).get_string().value();
        int64_t tick = parsePriceToTick(price_str, info.tick_decimals);
        uint64_t qty = parseQuantityToScaled(qty_str, info.qty_decimals);
        parsed_bids.emplace_back(tick, qty);
    }

    // Asks
    auto asks_arr = doc["asks"].get_array();
    parsed_asks.reserve(asks_arr.count_elements());
    for (auto ask : asks_arr)
    {
        auto price_qty = ask.get_array().value();
        std::string_view price_str = price_qty.at(0).get_string().value();
        std::string_view qty_str = price_qty.at(1).get_string().value();
        int64_t tick = parsePriceToTick(price_str, info.tick_decimals);
        uint64_t qty = parseQuantityToScaled(qty_str, info.qty_decimals);
        parsed_asks.emplace_back(tick, qty);
    }

    std::cout << "[Validator] parseSnapshot: parsed " << parsed_bids.size() << " bids and "
              << parsed_asks.size() << " asks" << std::endl;

    return true;
}

// ---------------------------------------------------------------------
// replayHistory() – Replay deltas from (S, F] with continuity check
// ---------------------------------------------------------------------
bool Validator::replayHistory(uint64_t S, uint64_t F, OrderBook &shadow)
{
    if (S == F)
    {
        // No deltas needed; snapshot is already at the frozen sequence.
        return true;
    }

    std::vector<Delta> deltas;
    if (!history_.getRange(S, F, deltas))
    {
        std::cerr << "[Validator] getRange(" << S << ", " << F << ") failed." << std::endl;
        return false;
    }

    std::cout << "[Replay] Replaying " << deltas.size() << " deltas from "
              << S << " to " << F << std::endl;

    if (deltas.empty())
    {
        std::cerr << "[Validator] No deltas in range despite S != F." << std::endl;
        return false;
    }

    uint64_t expected = S + 1;
    for (const auto &d : deltas)
    {
        if (d.U > expected)
        {
            std::cerr << "[Validator] Gap in history: expected U=" << expected
                      << " but got U=" << d.U << std::endl;
            return false;
        }
        expected = d.u + 1;
    }

    for (const auto &d : deltas)
    {
        applyDelta(d, shadow);
        auto snap = shadow.snapshotActiveLevels();
        std::cout << "[Shadow] After U=" << d.U << " u=" << d.u
                  << " bids=" << snap.bids.size()
                  << " asks=" << snap.asks.size()
                  << " best_bid=" << (snap.bids.empty() ? 0 : snap.bids[0].first)
                  << " best_ask=" << (snap.asks.empty() ? 0 : snap.asks[0].first)
                  << std::endl;
    }

    auto final_snap = shadow.snapshotActiveLevels();
    std::cout << "[Shadow] Final state after replay: "
              << "bids=" << final_snap.bids.size()
              << ", asks=" << final_snap.asks.size()
              << std::endl;

    return true;
}

bool Validator::buildShadowBook(ShadowResult &result)
{
    const auto &info = reconciler_.symbolInfo();

    RestResponse response = rest_.fetchDepthSnapshot(info.symbol, 5000);
    if (!response.ok())
    {
        std::cerr << "[Validator] REST snapshot failed. Status=" << response.status
                  << " body=" << response.body.substr(0, 200) << std::endl;
        return false;
    }

    uint64_t S = 0;
    std::vector<std::pair<int64_t, uint64_t>> parsed_bids, parsed_asks;
    if (!parseSnapshot(response.body, S, parsed_bids, parsed_asks))
    {
        std::cerr << "[Validator] Failed to parse snapshot." << std::endl;
        return false;
    }

    result.snapshot_seq = S;
    result.bid_depth = parsed_bids.size(); // Store bid depth separately
    result.ask_depth = parsed_asks.size(); // Store ask depth separately

    std::cout << "[Validator] Snapshot: seq=" << S
              << ", bids=" << result.bid_depth
              << ", asks=" << result.ask_depth
              << " (comparing bids up to " << result.bid_depth
              << ", asks up to " << result.ask_depth << ")" << std::endl;

    if (parsed_bids.empty() || parsed_asks.empty())
    {
        std::cerr << "[Validator] Snapshot has empty levels." << std::endl;
        return false;
    }

    int64_t best_bid = parsed_bids.front().first;
    int64_t best_ask = parsed_asks.front().first;

    result.book = std::make_unique<OrderBook>(best_bid, best_ask, window_size_);
    result.book->applySnapshot(parsed_bids, parsed_asks);

    std::cout << "[Shadow] Shadow  active bids: " << parsed_bids.size()
              << ", active asks: " << parsed_asks.size() << std::endl;

    return true;
}

// ---------------------------------------------------------------------
// compareBooks() – Compare only the depth exposed by REST snapshot
// Uses independent depths for bids and asks (stronger validation)
// ---------------------------------------------------------------------
bool Validator::compareBooks(
    const OrderBook &shadow,
    const std::vector<std::pair<int64_t, uint64_t>> &frozen_bids,
    const std::vector<std::pair<int64_t, uint64_t>> &frozen_asks,
    size_t snapshot_bid_depth,
    size_t snapshot_ask_depth,
    std::string &mismatch_details)
{
    auto shadow_snapshot = shadow.snapshotActiveLevels();

    const auto &shadow_bids = shadow_snapshot.bids;
    const auto &shadow_asks = shadow_snapshot.asks;

    //----------------------------------------------------------------------
    // Determine the comparable depth.
    //
    // Binance only exposed snapshot_bid_depth / snapshot_ask_depth levels.
    // We cannot validate deeper than that.
    //
    // Replay may legitimately delete some of those deepest levels, so we
    // compare only what exists in BOTH books.
    //----------------------------------------------------------------------

    const size_t compare_bid_depth =
        std::min({snapshot_bid_depth,
                  frozen_bids.size(),
                  shadow_bids.size()});

    const size_t compare_ask_depth =
        std::min({snapshot_ask_depth,
                  frozen_asks.size(),
                  shadow_asks.size()});

    //----------------------------------------------------------------------
    // Diagnostic logging only.
    // Losing tail levels after replay is expected if quantities became zero.
    //----------------------------------------------------------------------

    if (shadow_bids.size() < snapshot_bid_depth)
    {
        std::cout
            << "[Validator] Bid replay removed "
            << (snapshot_bid_depth - shadow_bids.size())
            << " tail level(s)." << std::endl;
    }

    if (shadow_asks.size() < snapshot_ask_depth)
    {
        std::cout
            << "[Validator] Ask replay removed "
            << (snapshot_ask_depth - shadow_asks.size())
            << " tail level(s)." << std::endl;
    }

    //----------------------------------------------------------------------
    // Compare bids.
    //----------------------------------------------------------------------

    for (size_t i = 0; i < compare_bid_depth; ++i)
    {

        if (frozen_bids[i] != shadow_bids[i])
        {
            std::cout << "[Validator] Bid level counts: frozen="
                      << frozen_bids.size() << ", shadow=" << shadow_bids.size() << std::endl;

            std::ostringstream oss;
            oss << "Bid mismatch at level " << i << ": frozen("
                << frozen_bids[i].first << "," << frozen_bids[i].second << ") shadow("
                << shadow_bids[i].first << "," << shadow_bids[i].second << ")\n";
            // Print surrounding levels for context
            oss << "  Surrounding levels (frozen):\n";
            for (int j = -10; j <= 10; ++j)
            {
                size_t idx = i + j;
                if (idx < frozen_bids.size())
                {
                    oss << "    [" << idx << "] (" << frozen_bids[idx].first << ","
                        << frozen_bids[idx].second << ")\n";
                }
            }
            oss << "  Surrounding levels (shadow):\n";
            for (int j = -10; j <= 10; ++j)
            {
                size_t idx = i + j;
                if (idx < shadow_bids.size())
                {
                    oss << "    [" << idx << "] (" << shadow_bids[idx].first << ","
                        << shadow_bids[idx].second << ")\n";
                }
            }
            mismatch_details = oss.str();
            return false;
        }
    }

    //----------------------------------------------------------------------
    // Compare asks.
    //----------------------------------------------------------------------

    for (size_t i = 0; i < compare_ask_depth; ++i)
    {

        if (frozen_asks[i] != shadow_asks[i])
        {
            std::cout << "[Validator] Ask level counts : frozen="
                      << frozen_asks.size() << ", shadow=" << shadow_asks.size() << std::endl;

            std::ostringstream oss;
            oss << "Ask mismatch at level " << i << ": frozen("
                << frozen_asks[i].first << "," << frozen_asks[i].second << ") shadow("
                << shadow_asks[i].first << "," << shadow_asks[i].second << ")\n";
            // Print surrounding levels for context
            oss << "  Surrounding levels (frozen):\n";
            for (int j = -5; j <= 5; ++j)
            {
                size_t idx = i + j;
                if (idx < frozen_asks.size())
                {
                    oss << "    [" << idx << "] (" << frozen_asks[idx].first << ","
                        << frozen_asks[idx].second << ")\n";
                }
            }
            oss << "  Surrounding levels (shadow):\n";
            for (int j = -5; j <= 5; ++j)
            {
                size_t idx = i + j;
                if (idx < shadow_asks.size())
                {
                    oss << "    [" << idx << "] (" << shadow_asks[idx].first << ","
                        << shadow_asks[idx].second << ")\n";
                }
            }
            mismatch_details = oss.str();
            return false;
        }
    }

    //----------------------------------------------------------------------
    // Success.
    //----------------------------------------------------------------------

    std::cout
        << "[Validator] Validation passed. Compared "
        << compare_bid_depth << "/" << snapshot_bid_depth
        << " bid levels and "
        << compare_ask_depth << "/" << snapshot_ask_depth
        << " ask levels."
        << std::endl;

    return true;
}
// ---------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------
std::chrono::system_clock::time_point Validator::lastValidationTime() const
{
    int64_t ms = last_validation_timestamp_ms_.load(std::memory_order_acquire);
    return std::chrono::time_point<std::chrono::system_clock>(
        std::chrono::milliseconds(ms));
}