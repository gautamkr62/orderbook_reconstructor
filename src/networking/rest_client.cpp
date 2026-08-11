#include "networking/rest_client.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <boost/beast/version.hpp>


// -----------------------------------------------------------------------------
// Internal shared state for one in-flight request. Lives as a shared_ptr so
// each async step can extend its own lifetime via shared_from_this() --
// standard Beast pattern for async chains.
// -----------------------------------------------------------------------------
struct RequestState : std::enable_shared_from_this<RequestState> {
    RestClient::ResponseCallback callback;

    tcp::resolver resolver;
    beast::ssl_stream<beast::tcp_stream> stream;
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::response<http::string_body> response;
    std::string host_;

    static constexpr auto CONNECT_TIMEOUT   = std::chrono::seconds(5);
    static constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(10);
    static constexpr auto WRITE_TIMEOUT     = std::chrono::seconds(10);
    static constexpr auto READ_TIMEOUT      = std::chrono::seconds(30);
    static constexpr auto SHUTDOWN_TIMEOUT  = std::chrono::seconds(5);

    RequestState(net::io_context& ioc, ssl::context& ssl_ctx, const std::string& host,
                 const std::string& target, RestClient::ResponseCallback cb)
        : callback(std::move(cb))
        , resolver(ioc)
        , stream(ioc, ssl_ctx)
        , host_(host)
    {
        // SNI -- required by Binance's edge for TLS routing.
        if (!SSL_set_tlsext_host_name(stream.native_handle(), host_.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }

        // Hostname verification. verify_peer (set on the ssl::context) only
        // checks the cert is signed by a trusted CA -- it does NOT check
        // that the cert's CN/SAN matches host_. This closes that gap.
        stream.set_verify_callback(ssl::host_name_verification(host_));

        request.version(11);
        request.method(http::verb::get);
        request.target(target);
        request.set(http::field::host, host_);
        request.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        request.set(http::field::accept, "application/json");
    }

    void start() {
        beast::get_lowest_layer(stream).expires_after(CONNECT_TIMEOUT);
        resolver.async_resolve(
            host_, "443",
            [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
                if (ec) return self->fail(ec, "resolve");
                self->onResolve(results);
            });
    }

    void onResolve(tcp::resolver::results_type results) {
        beast::get_lowest_layer(stream).expires_after(CONNECT_TIMEOUT);
        beast::get_lowest_layer(stream).async_connect(
            results,
            [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
                if (ec) return self->fail(ec, "connect");
                self->onConnect();
            });
    }

    void onConnect() {
        beast::get_lowest_layer(stream).expires_after(HANDSHAKE_TIMEOUT);
        stream.async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) return self->fail(ec, "SSL handshake");
                self->onSSLHandshake();
            });
    }

    void onSSLHandshake() {
        beast::get_lowest_layer(stream).expires_after(WRITE_TIMEOUT);
        http::async_write(
            stream, request,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->fail(ec, "write");
                self->onWrite();
            });
    }

    void onWrite() {
        beast::get_lowest_layer(stream).expires_after(READ_TIMEOUT);
        http::async_read(
            stream, buffer, response,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return self->fail(ec, "read");
                self->onRead();
            });
    }

    void onRead() {
        // Always hand back status + body -- the caller decides what counts
        // as success. We don't swallow non-200 responses here (Binance
        // returns e.g. {"code":-1121,"msg":"Invalid symbol."} with a
        // non-200 status; the caller needs to see both).
        RestResponse result;
        result.status = response.result_int();
        result.body = std::move(response.body());

        if (result.status != 200) {
            std::cerr << "RestClient: HTTP " << result.status
                      << " for " << host_ << std::endl;
        }

        if (callback) callback(std::move(result));

        // Best-effort graceful close; bounded by its own timeout so a
        // stalled shutdown doesn't leak this RequestState (and its socket)
        // indefinitely.
        beast::get_lowest_layer(stream).expires_after(SHUTDOWN_TIMEOUT);
        stream.async_shutdown(
            [self = shared_from_this()](beast::error_code ec) {
                // stream_truncated / eof on shutdown are routine for TLS
                // servers that close without a clean close_notify -- not
                // logged as errors.
                if (ec && ec != net::ssl::error::stream_truncated &&
                    ec != net::error::eof) {
                    std::cerr << "RestClient: shutdown warning: " << ec.message() << std::endl;
                }
            });
    }

    void fail(beast::error_code ec, const char* what) {
        std::cerr << "RestClient error: " << what << " : " << ec.message() << std::endl;

        RestResponse error_result;
        error_result.status = 0; // 0 = never got a real HTTP response
        error_result.body = std::string("RestClient error: ") + what + " : " + ec.message();

        if (callback) callback(std::move(error_result));
    }
};

// -----------------------------------------------------------------------------
// RestClient
// -----------------------------------------------------------------------------

RestClient::RestClient(net::io_context& ioc, const std::string& host)
    : ioc_(ioc)
    , ssl_ctx_(ssl::context::tlsv12_client)
    , host_(host) {
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    ssl_ctx_.set_default_verify_paths();
}

void RestClient::asyncRequest(const std::string& target, ResponseCallback callback) {
    // RequestState's constructor can throw (e.g. SSL_set_tlsext_host_name
    // failure) -- caught here so asyncRequest genuinely never throws
    // synchronously; every failure mode, including this one, goes through
    // the callback instead.
    std::shared_ptr<RequestState> state;
    try {
        state = std::make_shared<RequestState>(ioc_, ssl_ctx_, host_, target, callback);
    } catch (const std::exception& e) {
        if (callback) {
            callback(RestResponse{0, std::string("RestClient: request setup failed: ") + e.what()});
        }
        return;
    }

    // Post rather than call directly: guarantees start() runs on the thread
    // executing ioc_.run(), regardless of which thread called asyncRequest().
    net::post(ioc_, [state]() {
        state->start();
    });
}

// `target` must already be a fully-formed, validated path+query -- callers
// validate untrusted input (e.g. a symbol) before building `target`.
RestResponse RestClient::blockingFetch(const std::string& target) {
    auto runner_id = runner_thread_id_.load(std::memory_order_acquire);
    if (runner_id == std::thread::id{}) {
        throw std::logic_error("RestClient: setRunnerThreadId() was never called");
    }
    if (std::this_thread::get_id() == runner_id) {
        throw std::logic_error("RestClient: called from the io_context's own thread -- would deadlock");
    }

    // shared_ptr so the promise survives even if wait_for() times out below
    // and this stack frame returns before the (still in-flight) callback runs.
    auto promise = std::make_shared<std::promise<RestResponse>>();
    auto future = promise->get_future();

    asyncRequest(target, [promise](RestResponse response) {
        promise->set_value(std::move(response));
    });

    // Independent of the io_context's health -- if its thread dies or
    // stalls, Beast's own per-step timeouts (driven by that same
    // io_context) never fire either. Must exceed their sum:
    // CONNECT(5) + HANDSHAKE(10) + WRITE(10) + READ(30) = 55s, plus margin.
    constexpr auto HARD_CEILING = std::chrono::seconds(65);

    if (future.wait_for(HARD_CEILING) != std::future_status::ready) {
        return {0, "RestClient: hard ceiling exceeded -- io_context may be "
                    "dead, blocked, or extremely slow"};
    }
    return future.get();
}

// namespace {

// Binance symbols are always uppercase alnum (e.g. "DOGEUSDT"). Checking
// this before concatenating into the target avoids any query-string or
// header-injection surface without needing a general URL encoder.
// void validateSymbol(const std::string& symbol, const char* caller) {
//     if (symbol.empty()) {
//         throw std::invalid_argument(std::string(caller) + ": symbol is empty");
//     }
//     bool all_alnum = std::all_of(symbol.begin(), symbol.end(), [](unsigned char c) {
//         return std::isalnum(c) != 0;
//     });
//     if (!all_alnum) {
//         throw std::invalid_argument(
//             std::string(caller) + ": symbol contains non-alphanumeric characters");
//     }
// }

// } // namespace


RestResponse RestClient::fetchDepthSnapshot(const std::string& symbol) {
    // Default to 100 (Binance default)
    return fetchDepthSnapshot(symbol, 100);
}

RestResponse RestClient::fetchDepthSnapshot(const std::string& symbol, int limit) {
    // Validate symbol (only the symbol part)
    if (!std::all_of(symbol.begin(), symbol.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c));
        })) {
        throw std::invalid_argument("fetchDepthSnapshot: symbol contains non-alphanumeric characters");
    }

    std::string target = "/api/v3/depth?symbol=" + symbol;
    if (limit > 0) {
        target += "&limit=" + std::to_string(limit);
    }

    // Use the existing blockingFetch mechanism.
    return blockingFetch(target);
}
RestResponse RestClient::fetchExchangeInfo(const std::string& symbol) {
    // Validate symbol (only alnum)
    if (!std::all_of(symbol.begin(), symbol.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c));
        })) {
        throw std::invalid_argument("fetchExchangeInfo: symbol contains non-alphanumeric characters");
    }

    std::string target = "/api/v3/exchangeInfo?symbol=" + symbol;
    return blockingFetch(target);
}