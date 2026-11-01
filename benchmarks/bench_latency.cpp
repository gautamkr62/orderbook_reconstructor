// Deterministic latency benchmark for the engine hot path.
//
// Replays captured Binance diff-depth messages (benchmarks/data) through the
// production code — parseDelta(), applyDelta() on an OrderBook bootstrapped
// from the captured REST snapshot, DiffHistoryBuffer::push() — and reports
// ns per message and ns per price level, warm and with caches flushed before
// every message. Same input every run, so before/after numbers are comparable
// regardless of feed speed, network or scheduling.
//
// Also checks parseDelta() against an independent decoder (simdjson DOM +
// utils/price_parser) on every message, and times the two validator-side
// costs: freezing the live book and building a shadow book.
//
//   bench_latency [data_dir] [warm_reps]
#include "engine/apply_delta.hpp"
#include "engine/delta.hpp"
#include "engine/delta_parser.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/shadow_book.hpp"
#include "orderbook/orderbook.hpp"
#include "utils/json_levels.hpp"
#include "utils/price_parser.hpp"

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
inline int64_t ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

struct Corpus {
    const char* name;
    const char* snapshot_file;
    const char* stream_file;
    int tick_decimals;
    int qty_decimals;
    size_t window_size;  // same as src/main.cpp getConfigForSymbol()
};

const Corpus kCorpora[] = {
    {"BTCUSDT  @depth@100ms", "btcusdt_snapshot.json",  "btcusdt_depth_100ms.jsonl",   2, 5, 20'000'000},
    {"BTCUSDT  @depth      ", "btcusdt_snapshot.json",  "btcusdt_depth_1000ms.jsonl",  2, 5, 20'000'000},
    {"DOGEUSDT @depth@100ms", "dogeusdt_snapshot.json", "dogeusdt_depth_100ms.jsonl",  5, 0, 1'000'000},
    {"DOGEUSDT @depth      ", "dogeusdt_snapshot.json", "dogeusdt_depth_1000ms.jsonl", 5, 0, 1'000'000},
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

using Levels = std::vector<std::pair<int64_t, uint64_t>>;

struct Snapshot {
    uint64_t last_update_id = 0;
    Levels bids, asks;
};

Snapshot loadSnapshot(const std::string& path, int td, int qd) {
    Snapshot s;
    simdjson::padded_string json(readFile(path));
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(json).value();
    s.last_update_id = doc["lastUpdateId"];
    for (const char* side : {"bids", "asks"}) {
        Levels& out = side[0] == 'b' ? s.bids : s.asks;
        for (auto level : doc[side].get_array()) {
            std::string_view p, q;
            readPriceQty(level, p, q);
            out.emplace_back(parsePriceToTick(p, td), parseQuantityToScaled(q, qd));
        }
    }
    return s;
}

// Independent decoder used only to check parseDelta(): DOM API + the REST-path
// fixed-point parser, sharing no code with the hot-path parser.
bool referenceParse(const std::string& msg, int td, int qd, Delta& d) {
    static simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(msg).get(doc)) return false;
    d = Delta{};
    d.U = uint64_t(doc["U"]);
    d.u = uint64_t(doc["u"]);
    for (const char* side : {"b", "a"}) {
        auto& out = side[0] == 'b' ? d.bids : d.asks;
        for (auto level : doc[side].get_array()) {
            auto arr = level.get_array();
            out.emplace_back(parsePriceToTick(std::string_view(arr.at(0)), td),
                             parseQuantityToScaled(std::string_view(arr.at(1)), qd));
        }
    }
    return true;
}

bool sameDelta(const Delta& a, const Delta& b) {
    auto eq = [](const std::vector<LevelUpdate>& x, const std::vector<LevelUpdate>& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i].tick != y[i].tick || x[i].qty != y[i].qty) return false;
        return true;
    };
    return a.U == b.U && a.u == b.u && eq(a.bids, b.bids) && eq(a.asks, b.asks);
}

// Walk 64 MB so the next message starts with cold caches, like a live feed
// where the engine sits idle between messages.
std::vector<char> g_evict(64 << 20, 1);
volatile uint64_t g_sink = 0;
void evictCaches() {
    uint64_t s = 0;
    for (size_t i = 0; i < g_evict.size(); i += 64) { g_evict[i]++; s += static_cast<uint64_t>(g_evict[i]); }
    g_sink = g_sink + s;
}

struct Dist { double p50 = 0, p99 = 0, mean = 0; };
Dist dist(std::vector<int64_t> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (auto x : v) sum += static_cast<double>(x);
    return {static_cast<double>(v[v.size() / 2]),
            static_cast<double>(v[std::min(v.size() - 1, v.size() * 99 / 100)]),
            sum / static_cast<double>(v.size())};
}

struct StageSamples { std::vector<int64_t> parse, apply, push, total; };

void printRow(const char* stage, const std::vector<int64_t>& warm, const std::vector<int64_t>& cold, double levels) {
    const Dist w = dist(warm), c = dist(cold);
    std::printf("  %-8s | %8.0f %8.0f %8.0f | %8.0f %8.0f | %7.1f\n",
                stage, w.p50, w.p99, w.mean, c.p50, c.p99, w.mean / levels);
}

void runCorpus(const Corpus& c, const std::string& dir, int warm_reps) {
    // ---- load data
    const Snapshot snap = loadSnapshot(dir + "/" + c.snapshot_file, c.tick_decimals, c.qty_decimals);
    std::vector<std::string> raw;
    {
        std::ifstream in(dir + "/" + c.stream_file);
        for (std::string line; std::getline(in, line);)
            if (!line.empty()) raw.push_back(line);
    }

    // ---- correctness: parseDelta vs independent decoder, on every message
    size_t parse_mismatch = 0;
    std::vector<std::string> frames(raw.size());
    std::vector<Delta> parsed(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        fillFrameBuffer(frames[i], raw[i]);
        parseDelta(frames[i], raw[i].size(), c.tick_decimals, c.qty_decimals, parsed[i]);
        Delta ref;
        if (!referenceParse(raw[i], c.tick_decimals, c.qty_decimals, ref) || !sameDelta(parsed[i], ref))
            ++parse_mismatch;
    }

    // ---- keep the messages that continue the snapshot (Binance sync rule)
    std::vector<size_t> live;
    uint64_t seq = snap.last_update_id;
    bool bridged = false, chain_ok = true;
    for (size_t i = 0; i < parsed.size(); ++i) {
        const Delta& d = parsed[i];
        if (d.u <= seq) continue;
        if (!bridged) {
            if (d.U > seq + 1) { chain_ok = false; break; }
            bridged = true;
        } else if (d.U != seq + 1) { chain_ok = false; break; }
        live.push_back(i);
        seq = d.u;
    }
    if (live.empty()) {
        std::printf("%s: no messages continue the snapshot (lastUpdateId=%llu) — recapture data\n\n",
                    c.name, static_cast<unsigned long long>(snap.last_update_id));
        return;
    }

    double levels = 0;
    size_t bytes = 0;
    for (size_t i : live) { levels += parsed[i].bids.size() + parsed[i].asks.size(); bytes += raw[i].size(); }
    levels /= static_cast<double>(live.size());

    std::printf("%s | %zu msgs (%zu after snapshot), %.1f levels/msg, %.0f B/msg | parse check: %s | sequence: %s\n",
                c.name, raw.size(), live.size(), levels, double(bytes) / double(live.size()),
                parse_mismatch ? "MISMATCH" : "ok", chain_ok ? "gapless" : "GAP");
    if (parse_mismatch) std::printf("  !! %zu messages parsed differently from the reference decoder\n", parse_mismatch);

    // ---- engine hot path: parse -> apply -> history push
    auto makeBook = [&] {
        auto b = std::make_unique<OrderBook>(snap.bids.front().first, snap.asks.front().first, c.window_size, "BENCH");
        b->applySnapshot(snap.bids, snap.asks);
        return b;
    };
    auto book = makeBook();
    DiffHistoryBuffer history(1'000'000, 4'000'000);
    Delta delta;
    delta.bids.reserve(2048);
    delta.asks.reserve(2048);

    auto step = [&](size_t i, StageSamples& s) {
        const auto t0 = Clock::now();
        parseDelta(frames[i], raw[i].size(), c.tick_decimals, c.qty_decimals, delta);
        const auto t1 = Clock::now();
        applyDelta(delta, *book);
        const auto t2 = Clock::now();
        history.push(delta);
        const auto t3 = Clock::now();
        s.parse.push_back(ns(t0, t1));
        s.apply.push_back(ns(t1, t2));
        s.push.push_back(ns(t2, t3));
        s.total.push_back(ns(t0, t3));
    };

    StageSamples warm, cold, discard;
    for (size_t i : live) step(i, discard);  // warm-up pass
    for (int r = 0; r < warm_reps; ++r)
        for (size_t i : live) step(i, warm);
    book = makeBook();
    for (size_t i : live) { evictCaches(); step(i, cold); }

    std::printf("  stage    |  warm p50      p99     mean |  cold p50      p99 | ns/level (warm mean)\n");
    printRow("parse", warm.parse, cold.parse, levels);
    printRow("apply", warm.apply, cold.apply, levels);
    printRow("history", warm.push, cold.push, levels);
    printRow("TOTAL", warm.total, cold.total, levels);

    // ---- validator-side costs that touch the live engine
    std::vector<int64_t> freeze;
    for (int r = 0; r < 20; ++r) {
        const auto t0 = Clock::now();
        auto s = book->snapshotActiveLevels();
        const auto t1 = Clock::now();
        freeze.push_back(ns(t0, t1));
        g_sink = g_sink + s.bids.size();
    }
    // Validator's shadow book: REST snapshot + replay of the stream. It must
    // end up holding exactly what an OrderBook given the same input holds.
    ShadowBook shadow;
    std::vector<int64_t> shadow_build;
    for (int r = 0; r < 5; ++r) {
        const auto t0 = Clock::now();
        shadow.applySnapshot(snap.bids, snap.asks);
        for (size_t i : live) shadow.apply(parsed[i]);
        const auto t1 = Clock::now();
        shadow_build.push_back(ns(t0, t1));
    }
    auto reference = makeBook();
    for (size_t i : live) applyDelta(parsed[i], *reference);
    const auto ref = reference->snapshotActiveLevels();
    const bool shadow_ok = ref.bids == shadow.bids() && ref.asks == shadow.asks();
    const size_t shadow_bytes = (shadow.bids().capacity() + shadow.asks().capacity()) * sizeof(shadow.bids()[0]);

    std::printf("  validator: freeze copy p50 %.1f us | shadow build+replay %zu msgs p50 %.2f ms, %.0f KB "
                "(OrderBook-equivalent: %s)\n\n",
                dist(freeze).p50 / 1e3, live.size(), dist(shadow_build).p50 / 1e6,
                double(shadow_bytes) / 1024.0, shadow_ok ? "ok" : "MISMATCH");
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "benchmarks/data";
    const int warm_reps = argc > 2 ? std::atoi(argv[2]) : 50;
    std::printf("bench_latency: data=%s warm_reps=%d (times in ns unless noted; clock resolution ~%lld ns)\n\n",
                dir.c_str(), warm_reps,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::duration(1)).count()));
    for (const Corpus& c : kCorpora) runCorpus(c, dir, warm_reps);
    return 0;
}
