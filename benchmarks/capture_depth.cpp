// Capture raw Binance diff-depth WebSocket messages for the latency benchmark.
//
//   capture_depth <SYMBOL> <seconds> <out_dir>
//
// Writes <out_dir>/<symbol>_depth_100ms.jsonl and <symbol>_depth_1000ms.jsonl,
// one raw JSON message per line. Fetch a REST snapshot while it runs, e.g.
//   curl -s "https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000" > <out_dir>/btcusdt_snapshot.json
// so that its lastUpdateId falls inside the captured stream.
#include "networking/websocket_client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: capture_depth <SYMBOL> <seconds> <out_dir>\n";
        return 2;
    }
    std::string sym = argv[1];
    std::transform(sym.begin(), sym.end(), sym.begin(), [](unsigned char c) { return std::tolower(c); });
    const int secs = std::atoi(argv[2]);
    const std::string dir = argv[3];

    net::io_context ioc;
    auto guard = net::make_work_guard(ioc);
    WebSocketClient ws100(ioc), ws1000(ioc);
    std::ofstream f100(dir + "/" + sym + "_depth_100ms.jsonl");
    std::ofstream f1000(dir + "/" + sym + "_depth_1000ms.jsonl");
    std::atomic<int> n100{0}, n1000{0};
    ws100.setMessageCallback([&](std::string_view m) { f100 << m << '\n'; ++n100; });
    ws1000.setMessageCallback([&](std::string_view m) { f1000 << m << '\n'; ++n1000; });

    std::thread io([&] { ioc.run(); });
    ws100.connectAsync("/ws/" + sym + "@depth@100ms");
    ws1000.connectAsync("/ws/" + sym + "@depth");
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    ws100.forceClose();
    ws1000.forceClose();
    guard.reset();
    ioc.stop();
    io.join();
    std::cerr << "captured " << sym << ": 100ms=" << n100 << " 1000ms=" << n1000 << "\n";
}
