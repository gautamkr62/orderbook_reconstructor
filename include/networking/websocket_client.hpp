#ifndef WEBSOCKET_CLIENT_HPP
#define WEBSOCKET_CLIENT_HPP

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>

#include <functional>
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace beast = boost::beast;
namespace net   = boost::asio;
namespace ssl   = boost::asio::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

enum class CloseResult {
    Closed,
    Timeout
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Closing
};

class WebSocketClient {
public:
    using MessageCallback = std::function<void(std::string)>;
    using SimpleCallback  = std::function<void()>;
    using ErrorCallback   = std::function<void(const std::string&)>;

    WebSocketClient(net::io_context& ioc,
                    std::string host = "stream.binance.com",
                    std::string port = "9443");

    // Non-copyable
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // -------------------------------------------------------------------------
    // User callbacks – set directly before or after connecting
    // -------------------------------------------------------------------------
    SimpleCallback  on_open;
    SimpleCallback  on_close;
    MessageCallback on_message;
    ErrorCallback   on_error;

    // -------------------------------------------------------------------------
    // Connection management
    // -------------------------------------------------------------------------
    /** Start async connection. Returns false if already in a non‑Disconnected state. */
    bool connectAsync(const std::string& target);

    /** Graceful close – send WebSocket close frame. Must be Connected. */
    void disconnect();

    /** Forced close – tear down socket immediately. Works from Connecting or Connected. */
    void forceClose();

    /** Block until state becomes Disconnected or timeout expires. Thread‑safe. */
    CloseResult waitForClose(std::chrono::milliseconds timeout);

    /** Query current state (thread‑safe). */
    bool isConnected() const;
    bool isConnecting() const;

    void setMessageCallback(MessageCallback cb);
    void setOpenCallback(SimpleCallback cb);
    void setCloseCallback(SimpleCallback cb);
    void setErrorCallback(ErrorCallback cb);
    CloseResult waitForConnected(std::chrono::milliseconds timeout = std::chrono::seconds(10));

private:
    // Forward declaration – implementation hidden in .cpp
    struct Session;

    // State machine helpers
    void transitionTo(ConnectionState new_state);
    bool is_allowed_transition(ConnectionState from, ConnectionState to) const;

    net::io_context&          ioc_;
    ssl::context              ssl_ctx_;
    std::string               host_;
    std::string               port_;

    // Internal callbacks – update state and forward to user callbacks
    SimpleCallback  internal_open_cb_;
    std::function<void(Session*)> internal_close_cb_;  // now takes Session* to manage current_
    MessageCallback internal_msg_cb_;
    ErrorCallback   internal_error_cb_;

    // Active session (protected by current_mutex_)
    std::shared_ptr<Session> current_;
    mutable std::mutex       current_mutex_;

    // State tracking
    mutable std::mutex        state_mutex_;
    std::condition_variable   state_cv_;
    ConnectionState           state_{ConnectionState::Disconnected};
};

#endif // WEBSOCKET_CLIENT_HPP