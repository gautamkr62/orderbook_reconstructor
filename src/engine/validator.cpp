#include "engine/validator.hpp"

#include "utils/price_parser.hpp"
#include "engine/delta.hpp"
#include "utils/metrics.hpp"
#include "engine/apply_delta.hpp"
#include "engine/comparebooks.hpp"

#include <simdjson.h>

#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <iomanip>


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
        snapshot_ahead = false;

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

        const uint64_t S = shadow_result.boundary.last_update_id;

        // ---- Snapshot ahead of freeze? Not a failure, just an abort/retry. ----
        if (S > F)
        {
            snapshot_ahead = true;   // signal the caller for immediate retry
            size_t gap = S - F;
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
                    if (current_F >= S)
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

                // After waiting, check if live book actually caught up.
                if (F < S)
                {
                    // Still behind – cannot replay forward.
                    std::cout << "[Validator] Live book still behind snapshot (F=" << F
                              << " < S=" << S << "). Skipping this round." << std::endl;
                    updateTimestamp();
                    metrics_.incValidationSkipped();
                    return false;
                }
                // Now F >= S, proceed with replay and comparison.
            }
            else
            {
                // Large gap – skip the round (rare).
                std::cout << "[Validator] Large gap detected: " << gap << ". Skipping this round." << std::endl;
                updateTimestamp();
                metrics_.incValidationSkipped();
                return true;   // skip, not a failure
            }
        }

        // ---- Replay ----
        if (!replayHistory(S, F, *shadow_result.book))
        {
            updateTimestamp();
            metrics_.incValidationMismatch();
            return false;
        }

        // From here on, validation genuinely ran -- a false return below
        // means a REAL mismatch, not an abort.
        validation_completed = true;

        // ---- Drift telemetry (informational only, not correctness) ----
        ++validation_attempts_;

        auto shadow_snapshot = shadow_result.book->snapshotActiveLevels();

        size_t bid_tail_drift =
            shadow_result.boundary.bid_depth > shadow_snapshot.bids.size()
                ? shadow_result.boundary.bid_depth - shadow_snapshot.bids.size()
                : 0;
        size_t ask_tail_drift =
            shadow_result.boundary.ask_depth > shadow_snapshot.asks.size()
                ? shadow_result.boundary.ask_depth - shadow_snapshot.asks.size()
                : 0;

        if (bid_tail_drift > 0)
            ++bid_tail_drift_attempts_;
        if (ask_tail_drift > 0)
            ++ask_tail_drift_attempts_;

        max_bid_tail_drift_ = std::max(max_bid_tail_drift_, bid_tail_drift);
        max_ask_tail_drift_ = std::max(max_ask_tail_drift_, ask_tail_drift);

        // ---- Compare ----
        bool passed = compareBooks(
            *shadow_result.book,
            frozen_bids,
            frozen_asks,
            shadow_result.boundary,
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


    std::cout<<"In freeze function\n"; 
    if (!live_book_.verifyConsistency())
    {
        std::cerr << "[Validator] Live book consistency check failed! Skipping this validation round." << std::endl;
        return false;
    }

    auto snap = live_book_.snapshotActiveLevels();
    F = reconciler_.getLastAppliedSeq();
    // std::cout << "[Live] At sequence F=" << F
    //           << " bids=" << snap.bids.size()
    //           << " asks=" << snap.asks.size()
    //           << " best_bid=" << (snap.bids.empty() ? 0 : snap.bids[0].first)
    //           << " best_ask=" << (snap.asks.empty() ? 0 : snap.asks[0].first)
    //           << std::endl;



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
        // std::cout << "[Shadow] After U=" << d.U << " u=" << d.u
        //           << " bids=" << snap.bids.size()
        //           << " asks=" << snap.asks.size()
        //           << " best_bid=" << (snap.bids.empty() ? 0 : snap.bids[0].first)
        //           << " best_ask=" << (snap.asks.empty() ? 0 : snap.asks[0].first)
        //           << std::endl;
    }

    auto final_snap = shadow.snapshotActiveLevels();
    // std::cout << "[Shadow] Final state after replay: "
    //           << "bids=" << final_snap.bids.size()
    //           << ", asks=" << final_snap.asks.size()
    //           << std::endl;

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

    // ------------------------------------------------------------------
    // Safety margin: the last N ranks of a depth-limited REST snapshot
    // are subject to churn between independently-timed samples (a level
    // can flicker in/out of the top-5000 purely from normal order flow
    // between when live's last resync sampled REST and when this shadow
    // cycle's REST call fires). This is NOT a correctness bug in the
    // reconstructor -- comparing two honest, differently-timed samples
    // at the exact edge of a fixed-depth window will disagree sometimes.
    //
    // MARGIN VALUE: currently a placeholder. Before treating this as
    // final, run with [REST-RANK] logging for several hours and set
    // this to comfortably exceed the observed worst-case churn depth.
    // Confirmed observed case: a level churned all the way to rank 5000
    // (zero margin) in a live BTCUSDT session -- do not assume 200 is
    // sufficient without your own measurement.
    // ------------------------------------------------------------------
    constexpr size_t SAFETY_MARGIN = 100; 

    // Raw depth -- used for tail-drift telemetry, NOT for comparison boundary.
    result.boundary.last_update_id = S;
    result.boundary.bid_depth = parsed_bids.size();
    result.boundary.ask_depth = parsed_asks.size();

    // Margined comparison boundary -- this is what compareBooks() actually uses.
    const size_t safe_bid_count = parsed_bids.size() > SAFETY_MARGIN
        ? parsed_bids.size() - SAFETY_MARGIN
        : parsed_bids.size();
    const size_t safe_ask_count = parsed_asks.size() > SAFETY_MARGIN
        ? parsed_asks.size() - SAFETY_MARGIN
        : parsed_asks.size();

    if (safe_bid_count > 0) {
        result.boundary.worst_bid_price = parsed_bids[safe_bid_count - 1].first;
    } else if (!parsed_bids.empty()) {
        // Margin ate the whole side (tiny snapshot) -- fall back to full depth
        // rather than leaving worst_bid_price unset/stale.
        result.boundary.worst_bid_price = parsed_bids.back().first;
    }

    if (safe_ask_count > 0) {
        result.boundary.worst_ask_price = parsed_asks[safe_ask_count - 1].first;
    } else if (!parsed_asks.empty()) {
        result.boundary.worst_ask_price = parsed_asks.back().first;
    }

    std::cout << "[Validator] Snapshot: seq=" << result.boundary.last_update_id
              << ", bids=" << result.boundary.bid_depth
              << ", asks=" << result.boundary.ask_depth
              << " (compared bid_depth=" << safe_bid_count
              << ", compared ask_depth=" << safe_ask_count
              << ", worst bid=" << result.boundary.worst_bid_price
              << ", worst ask=" << result.boundary.worst_ask_price << ")" << std::endl;

    if (parsed_bids.empty() || parsed_asks.empty())
    {
        std::cerr << "[Validator] Snapshot has empty levels." << std::endl;
        return false;
    }

    int64_t best_bid = parsed_bids.front().first;
    int64_t best_ask = parsed_asks.front().first;

    result.book = std::make_unique<OrderBook>(best_bid, best_ask, window_size_, "SHADOW");
    result.book->applySnapshot(parsed_bids, parsed_asks);

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