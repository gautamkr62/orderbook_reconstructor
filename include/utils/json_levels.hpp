#pragma once

#include <stdexcept>
#include <string_view>

/**
 * Read one ["price", "qty"] pair from a simdjson On-Demand array element.
 *
 * On-Demand arrays are forward-only: array::at() may be called at most once
 * per array, so at(0) followed by at(1) is invalid. It happens to work in
 * builds with simdjson's development checks on, but fails in -DNDEBUG release
 * builds. Walk the elements once instead.
 */
template <typename Level>
inline void readPriceQty(Level&& level, std::string_view& price, std::string_view& qty)
{
    int idx = 0;
    for (auto field : level.get_array())
    {
        if (idx == 0)
            price = field.get_string();
        else if (idx == 1)
            qty = field.get_string();
        ++idx;
    }
    if (idx < 2)
        throw std::runtime_error("readPriceQty: level is not a [price, qty] pair");
}
