#include "WebSocket.h"

#include <cstring>
#include <optional>

#include "../utils/Utils.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace hope {

namespace net {

WebSocket::WebSocket(boost::asio::io_context& ioContext)
    : ioContext(ioContext)
    , sslContext(boost::asio::ssl::context::tlsv12_client)
    , resolver(ioContext)
    , webSocket(ioContext, sslContext)
    , asioConcurrentQueue(ioContext.get_executor()) {

    sslContext.set_options(
        boost::asio::ssl::context::default_workarounds |
        boost::asio::ssl::context::no_sslv2 |
        boost::asio::ssl::context::single_dh_use
        );
}

WebSocket::~WebSocket() {

    closeEvent();

}

boost::asio::awaitable<bool> WebSocket::connect(const std::string& host, const std::string& port, const std::string& path, const utils::Options& httpHeaders) {

    if (connecting.exchange(true)) {
        LOG_WARN("WebSocket::connect: handshake already in progress, skip duplicate connect");
        co_return false;
    }

    try {

        boost::asio::ip::tcp::resolver::results_type results = co_await resolver.async_resolve(
            host, port,
            boost::asio::cancel_after(connectTimeout, boost::asio::use_awaitable)
            );

        if (results.empty()) {
            throw std::runtime_error("resolve returned empty results (timeout or cancel)");
        }

        co_await boost::asio::async_connect(
            webSocket.next_layer().next_layer(),
            results,
            boost::asio::cancel_after(connectTimeout, boost::asio::use_awaitable)
            );

        co_await webSocket.next_layer().async_handshake(
            boost::asio::ssl::stream_base::client,
            boost::asio::cancel_after(connectTimeout, boost::asio::use_awaitable)
            );

        if (!httpHeaders.raw().empty()) {

            webSocket.set_option(boost::beast::websocket::stream_base::decorator(
                [httpHeaders](boost::beast::websocket::request_type& request) {
                    for (const boost::json::key_value_pair& item : httpHeaders.raw()) {
                        const std::string name(item.key());
                        const boost::json::value& value = item.value();
                        std::string text;
                        switch (value.kind()) {
                        case boost::json::kind::string:  text = value.as_string().c_str(); break;
                        case boost::json::kind::bool_:   text = value.as_bool() ? "true" : "false"; break;
                        case boost::json::kind::int64:   text = std::to_string(value.as_int64()); break;
                        case boost::json::kind::uint64:  text = std::to_string(value.as_uint64()); break;
                        case boost::json::kind::double_: text = std::to_string(value.as_double()); break;
                        default:                         text = boost::json::serialize(value); break;
                        }
                        request.set(name, text);
                    }
                }));
        }

        co_await webSocket.async_handshake(
            host, path,
            boost::asio::cancel_after(connectTimeout, boost::asio::use_awaitable)
            );

        asioConcurrentQueue.reset();

        setTcpKeepAlive(webSocket.next_layer().next_layer());

        asyncEvents.store(true);

        connecting.store(false);

        asyncEvent();

        if (onConnectHandle) {

            onConnectHandle();

        }

        co_return true;
    }
    catch (const std::exception& e) {

        connecting.store(false);

        bool aborted = false;
        if (const boost::system::system_error* systemError = dynamic_cast<const boost::system::system_error*>(&e)) {
            boost::system::error_code errorCode = systemError->code();
            aborted = errorCode == boost::asio::error::operation_aborted ||
                      errorCode == boost::asio::error::eof ||
                      errorCode == boost::asio::error::connection_aborted ||
                      errorCode == boost::asio::error::connection_reset;
        }
        if (aborted) LOG_WARN("WebSocket::connect aborted: %s", e.what());
        else LOG_ERROR("WebSocket::connect error: %s", e.what());

        if (onDisConnectHandle) {

            onDisConnectHandle();

        }

        co_return false;
    }
    catch (...) {

        connecting.store(false);

        LOG_ERROR("WebSocket::connect unknown error");

        if (onDisConnectHandle) {

            onDisConnectHandle();

        }

        co_return false;
    }
}

void WebSocket::closeEvent() {

    if (!asyncEvents.exchange(false)) {

        asioConcurrentQueue.close();

        closeWebSocket();

        return;
    }

    asioConcurrentQueue.close();

    closeWebSocket();
}

void WebSocket::asyncEvent() {

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->receiveCoroutine();
        co_return;
    }, boost::asio::detached);

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->writerCoroutine();
        co_return;
    }, boost::asio::detached);
}

boost::asio::awaitable<void> WebSocket::receiveCoroutine() {

    try {

        while (asyncEvents.load()) {

            boost::beast::flat_buffer buffer;

            co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

            std::string str = boost::beast::buffers_to_string(buffer.data());

            buffer.consume(buffer.size());

            if (onMessageHandle) {

                onMessageHandle(std::move(str));
            }
        }
    }
    catch (const std::exception& e) {

        LOG_ERROR("WebSocket receiveCoroutine error: %s", e.what());

        disConnectEvent();
    }
    catch (...) {

        LOG_ERROR("WebSocket receiveCoroutine unknown error");

        disConnectEvent();
    }

    co_return;
}

boost::asio::awaitable<void> WebSocket::writerCoroutine() {

    try {

        while (asyncEvents.load()) {

            std::optional<std::string> optional = co_await asioConcurrentQueue.dequeue();

            if (optional.has_value()) {

                std::string str = std::move(optional.value());

                co_await webSocket.async_write(boost::asio::buffer(str), boost::asio::use_awaitable);
            }
            else break;

            if (!asyncEvents.load()) break;

        }
    }
    catch (const std::exception& e) {

        LOG_ERROR("WebSocket writerCoroutine error: %s", e.what());

        disConnectEvent();
    }
    catch (...) {

        LOG_ERROR("WebSocket writerCoroutine unknown error");

        disConnectEvent();
    }

    co_return;
}

void WebSocket::disConnectEvent() {

    if (!asyncEvents.exchange(false)) return;

    asioConcurrentQueue.close();

    closeWebSocket();

    if (onDisConnectHandle) {

        onDisConnectHandle();

    }
}

void WebSocket::closeWebSocket() {

    boost::system::error_code errorCode;

    boost::asio::ip::tcp::socket& tcpSocket = webSocket.next_layer().next_layer();

    tcpSocket.cancel(errorCode);
    if (errorCode) {
        LOG_WARN("WebSocket::closeSocket cancel failed: %s", errorCode.message().c_str());
    }

    if (webSocket.is_open()) {
        try {
            webSocket.close(boost::beast::websocket::close_code::normal, errorCode);
        }
        catch (const std::exception& e) {
            LOG_ERROR("WebSocket::closeSocket close websocket failed: %s", e.what());
        }
    }

    if (tcpSocket.is_open()) {
        webSocket.next_layer().shutdown(errorCode);
        tcpSocket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, errorCode);
        tcpSocket.close(errorCode);
        if (errorCode && errorCode != boost::asio::error::not_connected) {
            LOG_ERROR("WebSocket::closeSocket close tcp failed: %s", errorCode.message().c_str());
        }
    }

    LOG_INFO("WebSocket is closed");
}

bool WebSocket::asyncWrite(std::string packet) {

    if (!asyncEvents.load()) {

        return false;

    }

    return asioConcurrentQueue.enqueue(std::move(packet));
}

bool WebSocket::isOpen() const {

    return webSocket.is_open();
}

std::string WebSocket::getRemoteAddress() {

    boost::system::error_code errorCode;

    boost::asio::ip::tcp::endpoint endpoint = webSocket.next_layer().next_layer().remote_endpoint(errorCode);

    if (errorCode) return "";

    return endpoint.address().to_string();
}

void WebSocket::setOnMessageHandle(std::function<void(std::string)> handle) {

    this->onMessageHandle = std::move(handle);
}

void WebSocket::setOnConnectHandle(std::function<void()> handle) {

    this->onConnectHandle = std::move(handle);
}

void WebSocket::setOnDisConnectHandle(std::function<void()> handle) {

    this->onDisConnectHandle = std::move(handle);
}

void WebSocket::setTcpKeepAlive(boost::asio::ip::tcp::socket& socket, int idle, int interval, int probes)
{
    boost::asio::detail::socket_type socketHandle = socket.native_handle();

    int keepAliveEnabled = 1;
    setsockopt(socketHandle, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&keepAliveEnabled), sizeof(keepAliveEnabled));

#if defined(__linux__)
    setsockopt(socketHandle, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(socketHandle, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(socketHandle, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));

#elif defined(_WIN32)
    struct tcp_keepalive keepAliveOption {};
    keepAliveOption.onoff = 1;
    keepAliveOption.keepalivetime = idle * 1000;
    keepAliveOption.keepaliveinterval = interval * 1000;
    DWORD bytesReturned = 0;
    WSAIoctl(socketHandle, SIO_KEEPALIVE_VALS,
             &keepAliveOption, sizeof(keepAliveOption),
             nullptr, 0, &bytesReturned, nullptr, nullptr);

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    setsockopt(socketHandle, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
    setsockopt(socketHandle, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#else
#warning "Unsupported platform, TCP keep-alive parameters not tuned"
#endif
}

}

}
