#pragma once

#include <cstdint>
#include <string_view>

int64_t parsePriceToTick(
    std::string_view str,
    int decimals
);

uint64_t parseQuantityToScaled(
    std::string_view str,
    int decimals
);