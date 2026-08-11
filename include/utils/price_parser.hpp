#pragma once

#include <string_view>
#include <cstdint>

/**
 * Parse a price string (e.g., "586.18000000") to a scaled integer tick.
 * @param str The price string from Binance JSON
 * @param decimals The number of decimal places for this symbol's tick size
 * @return Scaled integer representing the price in tick units
 * 
 * Example: parsePriceToTick("586.18", 2) -> 58618
 */
int64_t parsePriceToTick(std::string_view str, int decimals);

/**
 * Parse a quantity string (e.g., "39.71100000") to a scaled integer.
 * @param str The quantity string from Binance JSON
 * @param decimals The number of decimal places for this symbol's quantity step size
 * @return Scaled integer representing the quantity
 * 
 * Example: parseQuantityToScaled("39.711", 3) -> 39711
 */
uint64_t parseQuantityToScaled(std::string_view str, int decimals);