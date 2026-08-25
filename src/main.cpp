#include "orderbook/orderbook.hpp"
#include "networking/rest_client.hpp"
#include "networking/websocket_client.hpp"
#include "engine/reconciler.hpp"
#include "engine/validator.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/spscringqueue.hpp"
#include "engine/diff_history_buffer.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void signalHandler(int) {
    g_shutdown_requested.store(true, std::memory_order_release);
}

} // namespace


// -----------------------------------------------------------------------------
// SymbolPipeline – holds all components for one symbol
// -----------------------------------------------------------------------------
struct SymbolPipeline {
    using LiveQueue = SpscRingQueue<QueuedDelta, 8192>;

    std::string symbol;
    
    std::unique_ptr<OrderBook> book;
    std::unique_ptr<DiffHistoryBuffer> history;
    std::unique_ptr<LiveQueue> queue;
    std::unique_ptr<RestClient> rest;
    std::unique_ptr<WebSocketClient> ws;
    std::unique_ptr<Reconciler> reconciler;
    std::unique_ptr<Validator> validator;
    std::unique_ptr<MetricsCollector> metrics;   

    // Three threads for each symbol. 
    std::thread engine_thread;
    std::thread validator_thread;
    std::thread metrics_thread;

    SymbolPipeline(const std::string& sym,
                   size_t window_size,
                   size_t history_capacity,
                   boost::asio::io_context& ioc,
                   std::chrono::seconds validator_interval)
        : symbol(sym)
    {
        metrics   = std::make_unique<MetricsCollector>(sym);
        book      = std::make_unique<OrderBook>(0, 0, window_size, "LIVE");
        history   = std::make_unique<DiffHistoryBuffer>(history_capacity);
        queue     = std::make_unique<LiveQueue>();
        rest      = std::make_unique<RestClient>(ioc, "api.binance.com");
        ws        = std::make_unique<WebSocketClient>(ioc);

        reconciler = std::make_unique<Reconciler>(
            *book, *rest, *ws, *queue, *history, *metrics, window_size);
        validator  = std::make_unique<Validator>(
            *book, *history, *rest, *reconciler, *metrics, window_size, validator_interval);
    }
};




// -----------------------------------------------------------------------------
// Symbol-specific configuration
// -----------------------------------------------------------------------------
struct SymbolConfig {
    size_t window_size;
    size_t history_capacity;
    std::chrono::seconds validator_interval;
};

SymbolConfig getConfigForSymbol(const std::string& symbol) {
    if (symbol == "BTCUSDT" || symbol == "ETHUSDT") {
        // High-frequency symbols – bigger windows, larger buffers
        return {
            20'000'000,      // window_size
            1'000'000,       // history_capacity
            std::chrono::seconds(40)  // validator_interval
        };
    } else {
        // Lower-frequency symbols
        return {
            1'000'000,         // window_size
            1'000'000,       // history_capacity (could be smaller)
            std::chrono::seconds(30)  // validator_interval
        };
    }
}


int main(int argc, char* argv[]) {
    // -------------------------------------------------------------------------
    // Parse command line
    // -------------------------------------------------------------------------
    auto start_time = std::chrono::steady_clock::now();    bool benchmark_mode = false;
    int benchmark_duration = 60;
    std::vector<std::string> symbols;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--benchmark") {
            benchmark_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                benchmark_duration = std::stoi(argv[++i]);
            }
        } else {
            symbols.push_back(arg);
        }
    }

    // Default symbols if none provided
    if (symbols.empty()) {
        symbols = {"BTCUSDT", "ETHUSDT", "DOGEUSDT"};
    }

    // Benchmark mode runs only the first symbol
    // if (benchmark_mode) {
    //     symbols.resize(1);
    // }

    std::signal(SIGINT, signalHandler);

    std::cout << "=========================================\n";
    std::cout << " Binance Order Book Reconstructor\n";
    std::cout << "=========================================\n";

    // -------------------------------------------------------------------------
    // Global metrics (shared across symbols)
    // -------------------------------------------------------------------------
    MetricsCollector metrics;

    // -------------------------------------------------------------------------
    // Shared IO context and work guard
    // -------------------------------------------------------------------------
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);

    // -------------------------------------------------------------------------
    // Create pipelines
    // -------------------------------------------------------------------------
    std::vector<std::unique_ptr<SymbolPipeline>> pipelines;
    for (const auto& sym : symbols) {
        auto cfg = getConfigForSymbol(sym);
        if(sym == "BTCUSDT"){
            std::cout<<"Symbol is BTCUSDT\n"; 
            std::cout<<"It's size is \t"<< cfg.window_size<<"\n"; 
        }
        pipelines.push_back(std::make_unique<SymbolPipeline>(
            sym, cfg.window_size, cfg.history_capacity,
            ioc, cfg.validator_interval));
    }



    // -------------------------------------------------------------------------
    // Start IO thread (single shared IO for all symbols)
    // -------------------------------------------------------------------------
    std::thread io_thread([&ioc]() {
        // Pin to CPU 2 (physical core 1, leave sibling CPU 3 idle)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
            std::cerr << "[IO] Failed to pin to CPU 2\n";

        // Elevated priority — not SCHED_FIFO since I/O blocks anyway
        sched_param sp{ .sched_priority = 20 };
        if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) != 0)
            std::cerr << "[IO] Failed to set SCHED_RR (need CAP_SYS_NICE)\n";

        try {
            auto thread_id = std::this_thread::get_id();
            std::cout << "[IO] Thread started on CPU 2. ID: " << thread_id << "\n";
            ioc.run();
            std::cout << "[IO] Thread exited. ID: " << thread_id << "\n";
        } catch (const std::exception& e) {
            auto thread_id = std::this_thread::get_id();
            std::cerr << "[IO] Fatal exception on thread " << thread_id << ": " << e.what() << '\n';
        }
    });

    // Tell each RestClient which thread runs the event loop
    // 
    for (auto& p : pipelines) {
        p->rest->setRunnerThreadId(io_thread.get_id());
    }

    // -------------------------------------------------------------------------
    // Start engine threads (one per symbol)
    // -------------------------------------------------------------------------

    // This doesn't work when we have more than 3 symbols. 
    const int engine_cores[] = {4, 6, 8};  // one physical core per symbol

    if (pipelines.size() > std::size(engine_cores)) {
        std::cerr << "[Main] More pipelines than available engine cores!\n";
        return 1;
    }

    for (size_t i = 0; i < pipelines.size(); ++i) {
        auto& p = pipelines[i];
        int core_id = engine_cores[i];

        p->engine_thread = std::thread([p = p.get(), core_id]() {
            // ---- Pin to dedicated physical core ----
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(core_id, &cpuset);
            if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0)
                std::cerr << "[Engine] Failed to pin " << p->symbol 
                        << " to CPU " << core_id << "\n";

            // ---- Real-time priority ----
            sched_param sp{ .sched_priority = 80 };
            if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
                std::cerr << "[Engine] Failed to set SCHED_FIFO for " << p->symbol 
                        << " (need CAP_SYS_NICE or run as root)\n";

            // ---- Pre-fault thread-local buffers before first message ----
            prefaultEngineThreadLocals();

            auto thread_id = std::this_thread::get_id();
            try {
                std::cout << "[Engine] Thread for " << p->symbol
                        << " started on CPU " << core_id
                        << " ID: " << thread_id << "\n";
                p->reconciler->run(p->symbol);
                std::cout << "[Engine] Thread for " << p->symbol
                        << " exited normally. ID: " << thread_id << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[Engine] Thread for " << p->symbol
                        << " (ID: " << thread_id << ") fatal exception: " << e.what() << '\n';
            } catch (...) {
                std::cerr << "[Engine] Thread for " << p->symbol
                        << " (ID: " << thread_id << ") unknown fatal exception\n";
            }
        });
    }

    // -------------------------------------------------------------------------
    // Wait for all engines to initialize
    // -------------------------------------------------------------------------
    bool all_initialized = true;
    for (auto& p : pipelines) {
        std::cout << "[Main] Waiting for " << p->symbol << " to initialize...\n";
        if (!p->reconciler->waitUntilInitialized(std::chrono::seconds(30))) {
            std::cerr << "[Main] " << p->symbol << " init failed/timeout.\n";
            all_initialized = false;
            break;
        }
    }

    // -------------------------------------------------------------------------
    // Start validator threads only if all engines initialized
    // -------------------------------------------------------------------------
    if (all_initialized) {
        for (auto& p : pipelines) {
            p->validator_thread = std::thread([p = p.get()]() {
                try {
                    std::cout << "[Validator] Thread for " << p->symbol << " started.\n";
                    p->validator->run();
                } catch (const std::exception& e) {
                    std::cerr << "[Validator] Fatal exception: " << e.what() << '\n';
                }
            });
        }
    } else {
        std::cerr << "[Main] Not all engines initialized; skipping validators.\n";
        g_shutdown_requested.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // Metrics thread
    // -------------------------------------------------------------------------

    // creating metrics threads. 

   for (auto& p : pipelines) {
            p->metrics_thread = std::thread([p = p.get()]() {
            const std::string filename = p->symbol + "_metrics.txt";
            
            while (!g_shutdown_requested.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                
                // Print to console (uses default std::cout)
                p->metrics->printSnapshot();
                
                // Append to file
                std::ofstream out(filename);
                if (out) {
                    p->metrics->printSnapshot(out);  // <-- pass ostream
                    out.close();
                } else {
                    std::cerr << "[Metrics] Failed to open " << filename << std::endl;
                }
            }
        });
    }

    // -------------------------------------------------------------------------
    // Benchmark mode: run for specified duration then initiate shutdown
    // -------------------------------------------------------------------------
    // if (benchmark_mode) {
    //     std::cout << "Benchmark mode: running for " << benchmark_duration << " seconds..." << std::endl;
    //     std::this_thread::sleep_for(std::chrono::seconds(benchmark_duration));
    //     g_shutdown_requested.store(true, std::memory_order_release);
    // }

    // -------------------------------------------------------------------------
    // Wait for shutdown signal
    // -------------------------------------------------------------------------
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutdown requested...\n";

    // -------------------------------------------------------------------------
    // Cleanup (reverse dependency order)
    // -------------------------------------------------------------------------
    // 1. Stop validators
    for (auto& p : pipelines) {
        if (p->validator_thread.joinable()) {
            p->validator->stop();
        }
    }

    // 2. Stop reconcilers
    for (auto& p : pipelines) {
        p->reconciler->stop();
    }

    // 3. Disconnect WebSockets
    for (auto& p : pipelines) {
        p->ws->disconnect();
    }

    // 4. Stop IO context
    work_guard.reset();
    ioc.stop();

    // 5. Join validator threads
    for (auto& p : pipelines) {
        if (p->validator_thread.joinable()) {
            p->validator_thread.join();
        }
    }

    // 6. Join engine threads
    for (auto& p : pipelines) {
        if (p->engine_thread.joinable()) {
            p->engine_thread.join();
        }
    }

   
    // 7. Join IO thread
    if (io_thread.joinable()) {
        io_thread.join();
    }

    // 8. Join metrics thread

    for (auto& p : pipelines) {
        if (p->metrics_thread.joinable()) {
            p->metrics_thread.join();
        }
    }
    // if (metrics_thread.joinable()) {
    //     metrics_thread.join();
    // }



    // -------------------------------------------------------------------------
    // Print final metrics if benchmark mode
    // -------------------------------------------------------------------------
    if (benchmark_mode) {
    for (auto& p : pipelines) {
        auto s = p->metrics->snapshot();
        double throughput = s.delta_applied / benchmark_duration;

        std::string filename = p->symbol + "_benchmark.txt";
        std::ofstream out(filename);
        if (out) {
            out << "Symbol: " << p->symbol << "\n"
                << "Duration: " << benchmark_duration << " seconds\n"
                << "Throughput: " << throughput << " deltas/sec\n"
                << p->metrics->formatSnapshot();
        }
        std::cout << "[" << p->symbol << "] results written to " << filename << "\n";
    }
    }

    for (auto& p : pipelines) {
        auto s = p->metrics->snapshot();
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        double throughput = s.delta_applied / elapsed;

        std::string filename = p->symbol + "_benchmark.txt";
        std::ofstream out(filename);
        if (out) {
            out << "Symbol: " << p->symbol << "\n"
                << "Duration: " << elapsed << " seconds\n"
                << "Throughput: " << throughput << " deltas/sec\n"
                << p->metrics->formatSnapshot();
            out.close();
            std::cout << "[" << p->symbol << "] results written to " << filename << "\n";
        }
    }


    std::cout << "Shutdown complete.\n";
    return 0;
}