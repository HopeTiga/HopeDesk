#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include "AsioConcurrentQueue.h"
#include "../utils/Options.h"

namespace hope {

namespace net {

class WebSocket : public std::enable_shared_from_this<WebSocket> {
public:
    using WebSocketStream = boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

    explicit WebSocket(boost::asio::io_context& ioContext);

    ~WebSocket();

    WebSocket(const WebSocket&) = delete;

    WebSocket& operator=(const WebSocket&) = delete;

    boost::asio::awaitable<bool> connect(const std::string& host, const std::string& port,
                                         const std::string& path = "/",
                                         const utils::Options& httpHeaders = {});

    void closeEvent();

    bool asyncWrite(std::string packet);

    bool isOpen() const;

    std::string getRemoteAddress();

    void setOnMessageHandle(std::function<void(std::string)> handle);

    void setOnConnectHandle(std::function<void()> handle);

    void setOnDisConnectHandle(std::function<void()> handle);

private:

    void asyncEvent();

    boost::asio::awaitable<void> receiveCoroutine();

    boost::asio::awaitable<void> writerCoroutine();

    void disConnectEvent();

    void closeWebSocket();

    void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket, int idle = 30, int interval = 30, int probes = 30);

    boost::asio::io_context& ioContext;

    boost::asio::ssl::context sslContext;

    boost::asio::ip::tcp::resolver resolver;

    WebSocketStream webSocket;

    AsioConcurrentQueue<std::string> asioConcurrentQueue;

    std::atomic<bool> asyncEvents{ false };

    std::atomic<bool> connecting{ false };

    std::function<void(std::string)> onMessageHandle;

    std::function<void()> onConnectHandle;

    std::function<void()> onDisConnectHandle;

    static constexpr std::chrono::seconds connectTimeout{ 5 };
};

}

}
