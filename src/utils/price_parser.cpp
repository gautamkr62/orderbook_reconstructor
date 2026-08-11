#include "utils/price_parser.hpp"
#include <cstring>

static constexpr uint64_t POW10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

int64_t parsePriceToTick(std::string_view str, int decimals) {
    uint64_t int_part = 0, dec_part = 0;
    int dec_count = 0;
    bool in_dec = false;
    for (char c : str) {
        if (c == '.') { in_dec = true; continue; }
        if (in_dec) { dec_part = dec_part * 10 + (c - '0'); dec_count++; }
        else { int_part = int_part * 10 + (c - '0'); }
    }
    while (dec_count < decimals) { dec_part *= 10; dec_count++; }
    while (dec_count > decimals) { dec_part /= 10; dec_count--; }
    return static_cast<int64_t>(int_part * POW10[decimals] + dec_part);
}

uint64_t parseQuantityToScaled(std::string_view str, int decimals) {
    uint64_t int_part = 0, dec_part = 0;
    int dec_count = 0;
    bool in_dec = false;
    for (char c : str) {
        if (c == '.') { in_dec = true; continue; }
        if (in_dec) { dec_part = dec_part * 10 + (c - '0'); dec_count++; }
        else { int_part = int_part * 10 + (c - '0'); }
    }
    while (dec_count < decimals) { dec_part *= 10; dec_count++; }
    while (dec_count > decimals) { dec_part /= 10; dec_count--; }
    return int_part * POW10[decimals] + dec_part;
}