#pragma once

#include <vector>
#include <string>
#include <utility>
#include <cstdint>

#include "orderbook/orderbook.hpp"

/**
 * Compare two order book snapshots (frozen vs shadow) by price identity.
 * Only validates levels within the snapshot depth (or known range).
 * Returns true if all known levels match exactly; false otherwise,
 * filling mismatch_details with a description.
 */

bool compareBooks(const OrderBook &shadow,
                  const std::vector<std::pair<int64_t, uint64_t>> &frozen_bids,
                  const std::vector<std::pair<int64_t, uint64_t>> &frozen_asks,
                  size_t bid_depth,
                  size_t ask_depth,
                  std::string &mismatch_details); // <-- updated signature
