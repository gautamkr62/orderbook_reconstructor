#include "engine/comparebooks.hpp"

#include "orderbook/orderbook.hpp"
#include <unordered_map>
#include <sstream>

bool compareBooks(
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
//     std::cout << "Frozen bids:\n";
// for (const auto& [p, q] : frozen_bids)
//     std::cout << "  (" << p << ", " << q << ")\n";

// std::cout << "Shadow bids:\n";
// for (const auto& [p, q] : shadow_bids)
//     std::cout << "  (" << p << ", " << q << ")\n";

// std::cout << "snapshot_bid_depth = " << snapshot_bid_depth << '\n';
// std::cout << "frozen_bids.size() = " << frozen_bids.size() << '\n';
// std::cout << "shadow_bids.size() = " << shadow_bids.size() << '\n';


const size_t compare_bid_depth =
std::min({snapshot_bid_depth,
    frozen_bids.size(),
    shadow_bids.size()});
    // std::cout << "compare_bid_depth = " << compare_bid_depth << '\n';

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
