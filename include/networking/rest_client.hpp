#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// -----------------------------------------------------------------------------
// Shared namespace aliases. Kept here so WebSocketClient (added later) can
// reuse the same io_context / ssl::context types without re-declaring them.
// -----------------------------------------------------------------------------

// both do work. 
namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
using tcp       = boost::asio::ip::tcp;

// -----------------------------------------------------------------------------
// RestResponse: carries both the HTTP status and the body, so callers can
// distinguish "valid snapshot" from "error body that happens to parse as JSON"
// (e.g. Binance's {"code":-1121,"msg":"Invalid symbol."} error responses).
// -----------------------------------------------------------------------------
struct RestResponse {
    int status = 0;      // 0 means "request never completed" (network/timeout/misuse)
    std::string body;

    bool ok() const { return status == 200; }
};

// -----------------------------------------------------------------------------
// RestClient
//
// Does NOT run its own io_context -- something else must actively call
// ioc.run() (typically a dedicated background thread). Call
// setRunnerThreadId() once, right after spawning that thread: it enables
// deadlock detection for the blocking fetch* methods below.
//
// fetchDepthSnapshot()/fetchExchangeInfo() block the calling thread for up
// to ~65s worst case -- meant for one-shot startup calls (REST snapshot
// before subscribing to the WS diff stream), not steady-state use. They
// throw std::logic_error on setup misuse (no runner id set, or called from
// the runner thread itself) and std::invalid_argument on a bad symbol. For
// non-blocking calls, use asyncRequest() directly -- it never throws
// synchronously; failures arrive via the callback as RestResponse{0, ...}.
// -----------------------------------------------------------------------------
class RestClient {
public:
    using ResponseCallback = std::function<void(RestResponse)>;

    // `host` is the bare hostname, e.g. "api.binance.com" (no scheme, no path).
    // `ioc` must outlive this RestClient and must be actively run elsewhere.
    explicit RestClient(net::io_context& ioc, const std::string& host = "api.binance.com");

    // Holds a reference member (ioc_) -- copying and moving are both unsafe
    // to reason about generically, so both are disabled. Hold this by
    // pointer/reference wherever you need it to live.
    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;
    RestClient(RestClient&&) = delete;
    RestClient& operator=(RestClient&&) = delete;

    // Must be called once, immediately after spawning the thread that will
    // run ioc.run(). Safe to call from any thread, but must happen before
    // any blocking fetch* call, or those calls will throw std::logic_error.
    void setRunnerThreadId(std::thread::id id) {
        runner_thread_id_.store(id, std::memory_order_release);
    }

    // Fire-and-forget async GET. `target` is the path+query, e.g.
    // "/api/v3/depth?symbol=DOGEUSDT". Callback is invoked on whatever
    // thread is running ioc.run() -- do not assume it's the calling thread,
    // and do not block inside the callback.
    void asyncRequest(const std::string& target, ResponseCallback callback);

    // Convenience blocking wrappers built on top of asyncRequest(). Block
    // the calling thread for up to ~65s; see the class comment above.
    RestResponse fetchDepthSnapshot(const std::string& symbol);
    RestResponse fetchDepthSnapshot(const std::string& symbol, int limit = 100);
    RestResponse fetchExchangeInfo(const std::string& symbol);

private:
    // Deadlock guard + dispatch + hard-ceiling wait, shared by both fetch*
    // methods. `target` must already be validated -- this doesn't re-check it.
    RestResponse blockingFetch(const std::string& target);

    net::io_context& ioc_;
    ssl::context ssl_ctx_;
    std::string host_;
    std::atomic<std::thread::id> runner_thread_id_{};
};