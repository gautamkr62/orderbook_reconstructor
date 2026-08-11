#pragma once
#include <cstdint>

/**
 * Represents a single price level in the order book.
 * 
 * The price is NOT stored here. The position in the vector (index) implicitly
 * maps to the price via: price_tick = window.base_tick + index.
 * 
 * This keeps the struct small (8 bytes) for optimal cache locality.
 * No pointers, no price field — just the quantity.
 */
struct PriceLevel {
    // 0 means "this level is empty / inactive"
    // > 0 means the exact quantity currently resting at this price tick
    uint64_t quantity = 0;
};