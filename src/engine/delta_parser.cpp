#include "engine/delta_parser.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace {

thread_local simdjson::ondemand::parser tl_parser;

// ---------------------------------------------------------------
// Fixed-point decimal parse, straight from the JSON buffer.
//   p points at the first digit (just after the opening quote) and is
//   advanced past the number. Only the digits needed for `decimals` are
//   accumulated; extra precision is skipped, which truncates exactly like
//   dividing would but with no division and no overflow on digits we drop.
// ---------------------------------------------------------------
inline uint64_t parseFixed(const char*& p, int decimals)
{
    static constexpr uint64_t POW10[] = {
        1ULL, 10ULL, 100ULL, 1'000ULL, 10'000ULL,
        100'000ULL, 1'000'000ULL, 10'000'000ULL, 100'000'000ULL
    };
    uint64_t v = 0;
    while (static_cast<unsigned>(*p - '0') < 10) { v = v * 10 + static_cast<unsigned>(*p - '0'); ++p; }
    int d = 0;
    if (*p == '.') {
        ++p;
        while (d < decimals && static_cast<unsigned>(*p - '0') < 10) {
            v = v * 10 + static_cast<unsigned>(*p - '0'); ++p; ++d;
        }
        while (static_cast<unsigned>(*p - '0') < 10) ++p;  // unneeded precision
    }
    return v * POW10[decimals - d];
}

} // namespace

void fillFrameBuffer(std::string& buf, std::string_view frame)
{
    const size_t needed = frame.size() + simdjson::SIMDJSON_PADDING;
    if (buf.size() < needed)
        buf.resize(std::max(needed, kMinFrameBuffer));
    std::memcpy(buf.data(), frame.data(), frame.size());
    std::memset(buf.data() + frame.size(), 0, simdjson::SIMDJSON_PADDING);
}

// ---------------------------------------------------------------
// parseDelta
//   - zero-copy: the IO thread already padded the buffer
//   - sequence numbers extracted first
//   - each [price, qty] pair walked once with an iterator, numbers read
//     in place from the raw tokens
// ---------------------------------------------------------------
void parseDelta(const std::string& frame,
                size_t             len,
                int                tick_decimals,
                int                qty_decimals,
                Delta&             delta)
{
    simdjson::padded_string_view psv(frame.data(), len, frame.size());
    auto doc = tl_parser.iterate(psv);

    delta.bids.clear(); // retains vector capacity across calls
    delta.asks.clear();

    if (doc.find_field("U").get(delta.U))
        throw std::runtime_error("parseDelta: missing U");
    if (doc.find_field("u").get(delta.u))
        throw std::runtime_error("parseDelta: missing u");

    auto parse_side = [&](const char* key, std::vector<LevelUpdate>& levels)
    {
        auto arr_result = doc.find_field(key).get_array();
        if (arr_result.error()) return; // missing side is valid (empty)

        for (auto entry : arr_result.value_unsafe()) {
            auto pq = entry.get_array();
            if (pq.error()) continue;

            auto it = pq.value_unsafe().begin();

            // raw_json_token() points into the input buffer at the opening
            // quote: no unescape/copy into simdjson's string buffer.
            const char* price_p = (*it).value_unsafe().raw_json_token().data() + 1; ++it;
            const char* qty_p   = (*it).value_unsafe().raw_json_token().data() + 1;

            const int64_t  tick = static_cast<int64_t>(parseFixed(price_p, tick_decimals));
            const uint64_t qty  = parseFixed(qty_p, qty_decimals);

            levels.emplace_back(tick, qty);
        }
    };

    parse_side("b", delta.bids);
    parse_side("a", delta.asks);
}

void prefaultDeltaParser(int tick_decimals, int qty_decimals)
{
    Delta tmp;
    tmp.bids.reserve(2048);
    tmp.asks.reserve(2048);
    constexpr std::string_view dummy(R"({"U":1,"u":2,"b":[["0.01","1.0"]],"a":[["0.02","1.0"]]})");
    std::string frame;
    fillFrameBuffer(frame, dummy);
    parseDelta(frame, dummy.size(), tick_decimals, qty_decimals, tmp);
}
