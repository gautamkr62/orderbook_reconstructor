#include "networking/websocket_client.hpp"

#include <cassert>
#include <iostream>
#include <atomic>

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------
#define LOG_DEBUG(msg) std::cout << "[DEBUG][WSClient] " << msg << std::endl
#define LOG_WARN(msg)  std::cerr << "[WARN][WSClient] " << msg << std::endl

// ---------------------------------------------------------------------------
// Utility: state name for readable logs
// ---------------------------------------------------------------------------
static const char* stateToString(ConnectionState s) {
    switch (s) {
    case ConnectionState::Disconnected: return "Disconnected";
    case ConnectionState::Connecting:   return "Connecting";
    case ConnectionState::Connected:    return "Connected";
    case ConnectionState::Closing:      return "Closing";
    default:                            return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Session definition
// ---------------------------------------------------------------------------
struct WebSocketClient::Session : std::enable_shared_from_this<Session> {
    tcp::resolver resolver;
    websocket::stream<ssl::stream<tcp::socket>> ws;
    beast::flat_buffer buffer;

    std::string host;
    std::string port;
    std::string target;

    MessageCallback on_message;
    SimpleCallback  on_open;
    std::function<void(Session*)> on_close;
    ErrorCallback   on_error;

    std::atomic<bool> close_started_{false};
    std::atomic<bool> close_notified_{false};

    static constexpr auto CONNECT_TIMEOUT   = std::chrono::seconds(10);
    static constexpr auto HANDSHAKE_TIMEOUT = std::chrono::seconds(10);
    static constexpr auto READ_TIMEOUT      = std::chrono::minutes(60);
    static constexpr auto CLOSE_TIMEOUT     = std::chrono::seconds(5);

    Session(net::io_context& ioc,
            ssl::context& ssl_ctx,
            std::string host_,
            std::string port_,
            std::string target_,
            MessageCallback msg_cb,
            SimpleCallback open_cb,
            std::function<void(Session*)> close_cb,
            ErrorCallback err_cb)
        : resolver(ioc)
        , ws(ioc, ssl_ctx)
        , host(std::move(host_))
        , port(std::move(port_))
        , target(std::move(target_))
        , on_message(std::move(msg_cb))
        , on_open(std::move(open_cb))
        , on_close(std::move(close_cb))
        , on_error(std::move(err_cb))
    {
        // SNI and hostname verification
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()),
                                 net::error::get_ssl_category()};
            throw beast::system_error{ec};
        }
        ws.next_layer().set_verify_callback(ssl::host_name_verification(host));
        ws.read_message_max(8 * 1024 * 1024);
    }

    void start();
    void onResolve(tcp::resolver::results_type results);
    void onTcpConnect();
    void onSslHandshake();
    void onOpen();
    void readLoop();
    void onRead();
    void handleReadError(beast::error_code ec);
    void fail(beast::error_code ec, const char* what);
    void closeGracefully();
    void closeForced();
    void notifyClose();
};

// ---------------------------------------------------------------------------
// WebSocketClient implementation
// ---------------------------------------------------------------------------

WebSocketClient::WebSocketClient(net::io_context& ioc, std::string host, std::string port)
    : ioc_(ioc)
    , ssl_ctx_(ssl::context::tlsv12_client)
    , host_(std::move(host))
    , port_(std::move(port))
{
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
    ssl_ctx_.set_default_verify_paths();

    internal_open_cb_ = [this]() {
        transitionTo(ConnectionState::Connected);
        if (on_open) on_open();
    };

    internal_close_cb_ = [this](Session* s) {
        {
            std::lock_guard<std::mutex> lock(current_mutex_);
            if (current_.get() == s) {
                current_.reset();
            }
        }
        transitionTo(ConnectionState::Disconnected);
        if (on_close) on_close();
    };

    internal_error_cb_ = [this](const std::string& err) {
        if (on_error) on_error(err);
    };

    internal_msg_cb_ = [this](std::string msg) {
        if (on_message) on_message(std::move(msg));
    };
}

bool WebSocketClient::connectAsync(const std::string& target) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != ConnectionState::Disconnected) {
            LOG_WARN("connectAsync called while state is " +
                     std::string(stateToString(state_)) +
                     ". Only Disconnected state is allowed.");
            return false;
        }
    }

    transitionTo(ConnectionState::Connecting);

    std::shared_ptr<Session> session;
    try {
        session = std::make_shared<Session>(
            ioc_, ssl_ctx_, host_, port_, target,
            internal_msg_cb_, internal_open_cb_,
            internal_close_cb_, internal_error_cb_
        );
    } catch (const std::exception& e) {
        internal_error_cb_(std::string("connect setup failed: ") + e.what());
        internal_close_cb_(nullptr);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(current_mutex_);
        current_ = session;
    }

    net::post(ioc_, [session]() {
        session->start();
    });
    return true;
}

void WebSocketClient::disconnect() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(current_mutex_);
        session = current_;
    }

    if (!session) {
        transitionTo(ConnectionState::Disconnected);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != ConnectionState::Connected) {
            LOG_DEBUG("disconnect() ignored: state is " +
                      std::string(stateToString(state_)));
            return;
        }
    }

    transitionTo(ConnectionState::Closing);
    net::post(ioc_, [session]() {
        session->closeGracefully();
    });
}

void WebSocketClient::forceClose() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(current_mutex_);
        session = current_;
    }

    if (!session) {
        transitionTo(ConnectionState::Disconnected);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ != ConnectionState::Connected &&
            state_ != ConnectionState::Connecting) {
            LOG_DEBUG("forceClose() ignored: state is " +
                      std::string(stateToString(state_)));
            return;
        }
    }

    transitionTo(ConnectionState::Closing);
    net::post(ioc_, [session]() {
        session->closeForced();
    });
}

CloseResult WebSocketClient::waitForClose(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    bool ready = state_cv_.wait_for(lock, timeout, [this] {
        return state_ == ConnectionState::Disconnected;
    });
    return ready ? CloseResult::Closed : CloseResult::Timeout;
}

CloseResult WebSocketClient::waitForConnected(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    bool ready = state_cv_.wait_for(lock, timeout, [this] {
        return state_ == ConnectionState::Connected ||
               state_ == ConnectionState::Disconnected;
    });
    if (!ready) return CloseResult::Timeout;
    return (state_ == ConnectionState::Connected) ? CloseResult::Closed : CloseResult::Timeout;
}

bool WebSocketClient::isConnected() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == ConnectionState::Connected;
}

bool WebSocketClient::isConnecting() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == ConnectionState::Connecting;
}

void WebSocketClient::setMessageCallback(MessageCallback cb) {
    on_message = std::move(cb);
}

void WebSocketClient::setOpenCallback(SimpleCallback cb) {
    on_open = std::move(cb);
}

void WebSocketClient::setCloseCallback(SimpleCallback cb) {
    on_close = std::move(cb);
}

void WebSocketClient::setErrorCallback(ErrorCallback cb) {
    on_error = std::move(cb);
}

// ---------------------------------------------------------------------------
// Private state machine helpers
// ---------------------------------------------------------------------------

void WebSocketClient::transitionTo(ConnectionState new_state) {
    std::unique_lock<std::mutex> lock(state_mutex_);

    if (!is_allowed_transition(state_, new_state)) {
        LOG_WARN("Invalid state transition: " +
                 std::string(stateToString(state_)) + " -> " +
                 stateToString(new_state));
        return;
    }

    LOG_DEBUG("State: " + std::string(stateToString(state_)) +
              " -> " + stateToString(new_state));

    state_ = new_state;

    if (new_state == ConnectionState::Disconnected) {
        lock.unlock();
        state_cv_.notify_all();
    }
}

bool WebSocketClient::is_allowed_transition(ConnectionState from, ConnectionState to) const {
    switch (from) {
    case ConnectionState::Disconnected:
        return to == ConnectionState::Disconnected ||
               to == ConnectionState::Connecting;
    case ConnectionState::Connecting:
        return to == ConnectionState::Connected ||
               to == ConnectionState::Disconnected ||
               to == ConnectionState::Closing;
    case ConnectionState::Connected:
        return to == ConnectionState::Closing ||
               to == ConnectionState::Disconnected;
    case ConnectionState::Closing:
        return to == ConnectionState::Disconnected;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Session implementation (CORRECTED for your Boost version)
// ---------------------------------------------------------------------------

void WebSocketClient::Session::start() {
    // Removed expires_after() – not supported on this socket type
    resolver.async_resolve(
        host, port,
        [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) return self->fail(ec, "resolve");
            self->onResolve(results);
        });
}

void WebSocketClient::Session::onResolve(tcp::resolver::results_type results) {
    auto endpoint = *results.begin();  // take the first resolved endpoint
    beast::get_lowest_layer(ws).async_connect(
        endpoint,
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) return self->fail(ec, "connect");
            self->onTcpConnect();
        });
}

void WebSocketClient::Session::onTcpConnect() {
    ws.next_layer().async_handshake(
        ssl::stream_base::client,
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) return self->fail(ec, "ssl_handshake");
            self->onSslHandshake();
        });
}

void WebSocketClient::Session::onSslHandshake() {
    ws.async_handshake(
        host, target,
        [self = shared_from_this()](beast::error_code ec) {
            if (ec) return self->fail(ec, "ws_handshake");
            self->onOpen();
        });
}

void WebSocketClient::Session::onOpen() {
    LOG_DEBUG("Connected to " + host + ":" + port + target);
    if (on_open) on_open();
    readLoop();
}

void WebSocketClient::Session::readLoop() {
    ws.async_read(
        buffer,
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) return self->handleReadError(ec);
            self->onRead();
        });
}

void WebSocketClient::Session::onRead() {
    std::string msg = beast::buffers_to_string(buffer.data());
    buffer.consume(buffer.size());

    // LOG_DEBUG("Received message of size " + std::to_string(msg.size()) + " bytes");
    if (on_message) on_message(std::move(msg));

    readLoop();
}

void WebSocketClient::Session::handleReadError(beast::error_code ec) {
    if (ec == websocket::error::closed ||
        ec == net::error::eof ||
        ec == net::ssl::error::stream_truncated) {
        LOG_DEBUG("Connection closed by remote or network.");
        notifyClose();
        return;
    }
    fail(ec, "read");
}

void WebSocketClient::Session::fail(beast::error_code ec, const char* what) {
    LOG_WARN(std::string("Error: ") + what + " : " + ec.message());
    if (on_error) on_error(std::string(what) + ": " + ec.message());
    notifyClose();
}

void WebSocketClient::Session::closeGracefully() {
    if (!close_started_.exchange(true)) {
        LOG_DEBUG("Initiating graceful close...");
        ws.async_close(
            websocket::close_code::normal,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec && ec != net::error::eof &&
                    ec != net::ssl::error::stream_truncated &&
                    ec != websocket::error::closed) {
                    LOG_WARN("graceful close error: " + ec.message());
                }
                self->notifyClose();
            });
    }
}

void WebSocketClient::Session::closeForced() {
    if (!close_started_.exchange(true)) {
        LOG_DEBUG("Initiating forced close...");
        beast::error_code ec;
        ws.next_layer().shutdown(ec);
        ws.next_layer().next_layer().close(ec);
        notifyClose();
    }
}

void WebSocketClient::Session::notifyClose() {
    if (!close_notified_.exchange(true)) {
        LOG_DEBUG("Transport terminated, notifying client.");
        if (on_close) on_close(this);
    }
}