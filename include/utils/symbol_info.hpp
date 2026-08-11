#pragma once

#include <string>

struct SymbolInfo {
    std::string symbol;
    int tick_decimals = 0;
    int qty_decimals = 0;
};