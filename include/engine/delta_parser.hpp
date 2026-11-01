#pragma once

#include "engine/delta.hpp"

#include <cstddef>
#include <string>
#include <string_view>

/**
 * Binance diff-depth JSON -> Delta.
 *
 * Shared by the Reconciler (live path) and benchmarks/bench_latency.cpp, so the
 * benchmark always measures the code that runs in production.
 */

// Frame buffers start at this size so typical depth messages never force a
// regrow; larger frames grow a buffer once and it keeps that capacity.
constexpr size_t kMinFrameBuffer = 32 * 1024;

// Copy a frame into a reusable buffer as [frame][SIMDJSON_PADDING zero bytes],
// the layout parseDelta() parses in place. Allocates only when `buf` is
// smaller than anything it held before.
void fillFrameBuffer(std::string& buf, std::string_view frame);

// Parse a buffer produced by fillFrameBuffer(): JSON in [0, len), followed by
// padding. Prices/quantities become fixed-point integers with the given number
// of decimals. Reuses delta's vector capacity. Throws std::runtime_error if
// the sequence fields are missing.
void parseDelta(const std::string& frame,
                size_t             len,
                int                tick_decimals,
                int                qty_decimals,
                Delta&             delta);

// Warm the calling thread's parser state so the first real message does not
// pay for simdjson's internal allocations.
void prefaultDeltaParser(int tick_decimals, int qty_decimals);
