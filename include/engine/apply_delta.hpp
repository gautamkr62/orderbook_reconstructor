#pragma once

#include "engine/delta.hpp"
#include "orderbook/orderbook.hpp"

/**
 * Apply a parsed Delta to an OrderBook.
 * Used by both the Reconciler (live updates) and the Validator (shadow replay).
 */
void applyDelta(const Delta& delta, OrderBook& book);