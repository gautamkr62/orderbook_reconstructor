#include "orderbook/orderbook.hpp"
#include "networking/rest_client.hpp"
#include "networking/websocket_client.hpp"
#include "engine/reconciler.hpp"
#include "engine/validator.hpp"
#include "engine/diff_history_buffer.hpp"
#include "engine/threadsafe_queue.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> g_shutdown_requested{false};


// There is a signalhandler thread? 
void signalHandler(int) {
    g_shutdown_requested.store(true, std::memory_order_release);
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);

    std::cout << "=========================================\n";
    std::cout << " Binance Order Book Reconstructor\n";
    std::cout << "=========================================\n";

    // ---Collect Metrics---------------------------------------------------
    MetricsCollector metrics;


    //----------------------------------------------------------------------
    // 1. ASIO Context & Work Guard
    //----------------------------------------------------------------------
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);

    //----------------------------------------------------------------------
    // 2. Networking Clients
    //----------------------------------------------------------------------
    RestClient rest_client(ioc, "api.binance.com");
    WebSocketClient ws_client(ioc);

    //----------------------------------------------------------------------
    // 3. SPSC Queue (IO → Engine)
    //---------------------------------------------------------------------
    std::string symbol = "BTCUSDT";
        if (argc > 1) {
            symbol = argv[1];
        }
    size_t queue_capacity = 8192;
    size_t history_capacity = 50000;
    if (symbol == "BTCUSDT" || symbol == "ETHUSDT") {
        queue_capacity = 65536;      // 4x larger
        history_capacity = 1000000;   // 3x larger
    }
    
    
    ThreadSafeQueue<std::string> message_queue(queue_capacity);

    //----------------------------------------------------------------------
    // 4. Diff History Buffer (Engine → Validator)
    //----------------------------------------------------------------------


    
    DiffHistoryBuffer history_buffer(history_capacity);

    //----------------------------------------------------------------------
    // 5. OrderBook (live)
    //----------------------------------------------------------------------
    
    size_t window_size = 1'000'000;
    if (symbol == "BTCUSDT" || symbol == "ETHUSDT") {
        window_size = 10'000'000;   // 10 million ticks (covers price range up to $100k with 0.01 tick)
    }
    OrderBook book(0, 0, window_size);

    //----------------------------------------------------------------------
    // 6. Reconciler (Engine logic)
    //----------------------------------------------------------------------
    Reconciler reconciler(book, rest_client, ws_client, message_queue, history_buffer, metrics, window_size);

    //----------------------------------------------------------------------
    // 7. Validator (Background auditor) – constructed but not yet started
    //----------------------------------------------------------------------


    std::chrono::seconds validator_interval = std::chrono::seconds(30);
    if (symbol == "BTCUSDT" || symbol == "ETHUSDT") {
        validator_interval = std::chrono::seconds(40);   // 40 seconds 
        // will update it to 2 minutes later. 
    }
    Validator validator(book,
                        history_buffer,
                        rest_client,
                        reconciler,
                        metrics, 
                        window_size,
                        validator_interval);

    //----------------------------------------------------------------------
    // 8. Start IO Thread
    //----------------------------------------------------------------------

 

    std::thread io_thread([&ioc]() {
        try {
            std::cout << "[IO] Thread started.\n";
            ioc.run();
            std::cout << "[IO] Thread exited.\n";
        } catch (const std::exception& e) {
            std::cerr << "[IO] Fatal exception: " << e.what() << '\n';
        }
    });

    // CRITICAL: Tell the RestClient which thread runs the event loop.
    // Prevents deadlock if fetchDepthSnapshot() is called from the same thread.
    rest_client.setRunnerThreadId(io_thread.get_id());

    //----------------------------------------------------------------------
    // 9. Start Engine Thread
    //----------------------------------------------------------------------
    std::thread engine_thread([&reconciler, symbol ]() {
        try {
            std::cout << "[Engine] Thread started.\n";
            reconciler.run(symbol);
        } catch (const std::exception& e) {
            std::cerr << "[Engine] Fatal exception: " << e.what() << '\n';
        }
    });

    //----------------------------------------------------------------------
    // 10. Wait for Engine to initialize (with timeout)
    //----------------------------------------------------------------------
    bool engine_initialized = false;
    std::cout << "[Main] Waiting for Engine to initialize..." << std::endl;
    if (!reconciler.waitUntilInitialized(std::chrono::seconds(30))) {
        std::cerr << "[Main] Engine initialization failed or timed out.\n";
        g_shutdown_requested.store(true, std::memory_order_release);
        engine_initialized = false;
    } else {
        std::cout << "[Main] Engine initialized. Starting Validator.\n";
        engine_initialized = true;
    }

    //----------------------------------------------------------------------
    // 11. Start Validator Thread (only if Engine initialized)
    //----------------------------------------------------------------------
    std::thread validator_thread;
    if (engine_initialized) {
        validator_thread = std::thread([&validator]() {
            try {
                std::cout << "[Validator] Thread started in main.\n";
                validator.run();
            } catch (const std::exception& e) {
                std::cerr << "[Validator] Fatal exception: " << e.what() << '\n';
            }
        });
    } else {
        std::cout << "[Main] Skipping Validator startup due to Engine failure.\n";
    }

    // Metrics thread running 
        
    std::thread metrics_thread([&metrics]() {
        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            metrics.printSnapshot();
        }
    });



    //----------------------------------------------------------------------
    // 12. Wait for Shutdown Signal (Ctrl+C) or Engine failure
    //----------------------------------------------------------------------
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nShutdown requested...\n";

    //----------------------------------------------------------------------
    // 13. Cleanup (Reverse dependency order)
    //----------------------------------------------------------------------
    // 13a. Stop the Validator first (wakes from sleep)
    if (validator_thread.joinable()) {
        validator.stop();
    }

    // 13b. Stop the Engine (unblocks queue)
    reconciler.stop();

    // 13c. Close the WebSocket (allows IO thread to finish)
    ws_client.disconnect();

    // 13d. Stop the IO context
    work_guard.reset();
    ioc.stop();

    // 13e. Join all threads (reverse creation order)
    if (validator_thread.joinable())
        validator_thread.join();

    if (engine_thread.joinable())
        engine_thread.join();

    if (io_thread.joinable())
        io_thread.join();
    if (metrics_thread.joinable()) 
        metrics_thread.join();


    std::cout << "Shutdown complete.\n";
    return 0;
}