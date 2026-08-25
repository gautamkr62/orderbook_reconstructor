#include "utils/price_parser.hpp"

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace {

constexpr uint64_t POW10[] = {
    1ULL,
    10ULL,
    100ULL,
    1'000ULL,
    10'000ULL,
    100'000ULL,
    1'000'000ULL,
    10'000'000ULL,
    100'000'000ULL
};

uint64_t parseScaled(std::string_view str, int decimals)
{
    if (decimals < 0 || decimals > 8)
        throw std::invalid_argument(
            "parseScaled: invalid decimal precision"
        );

    uint64_t int_part = 0;
    uint64_t dec_part = 0;
    int dec_count = 0;
    bool in_dec = false;

    for (char c : str)
    {
        if (c == '.')
        {
            if (in_dec)
                throw std::invalid_argument(
                    "parseScaled: multiple decimal points"
                );

            in_dec = true;
            continue;
        }

        if (c < '0' || c > '9')
            throw std::invalid_argument(
                "parseScaled: invalid numeric value"
            );

        const uint64_t digit =
            static_cast<unsigned>(c - '0');

        if (in_dec)
        {
            dec_part = dec_part * 10 + digit;
            ++dec_count;
        }
        else
        {
            int_part = int_part * 10 + digit;
        }
    }

    while (dec_count < decimals)
    {
        dec_part *= 10;
        ++dec_count;
    }

    while (dec_count > decimals)
    {
        dec_part /= 10;
        --dec_count;
    }

    return int_part * POW10[decimals] + dec_part;
}

} // namespace

int64_t parsePriceToTick(std::string_view str, int decimals)
{
    return static_cast<int64_t>(
        parseScaled(str, decimals)
    );
}

uint64_t parseQuantityToScaled(std::string_view str, int decimals)
{
    return parseScaled(str, decimals);
}