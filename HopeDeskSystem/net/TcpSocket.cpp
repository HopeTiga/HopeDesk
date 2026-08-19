#include "TcpSocket.h"

#include <array>
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

TcpSocket::TcpSocket(boost::asio::io_context& ioContext)
    : ioContext(ioContext)
    , tcpSocket(ioContext)
    , asioConcurrentQueue(ioContext.get_executor()) {
}

TcpSocket::~TcpSocket() {
    closeEvent();
}

boost::asio::awaitable<bool> TcpSocket::connect(unsigned short port) {
    if (connecting.exchange(true)) {
        LOG_WARN("TcpSocket::connect: already connecting, skip duplicate connect");
        co_return false;
    }

    try {

        boost::asio::ip::address address = boost::asio::ip::make_address("127.0.0.1");

        boost::asio::ip::tcp::endpoint endpoint(address, port);

        co_await boost::asio::async_connect(tcpSocket, std::array{ endpoint }, boost::asio::use_awaitable);

        asioConcurrentQueue.reset();

        setTcpKeepAlive(tcpSocket);

        asyncEvents.store(true);

        connecting.store(false);

        startCoroutines();

        LOG_INFO("TcpSocket connected to 127.0.0.1:%u", static_cast<unsigned int>(port));

        co_return true;
    }
    catch (const std::exception& e) {
        connecting.store(false);
        LOG_ERROR("TcpSocket::connect error: %s", e.what());
        closeSocket();
        co_return false;
    }
    catch (...) {
        connecting.store(false);
        LOG_ERROR("TcpSocket::connect unknown error");
        closeSocket();
        co_return false;
    }
}

void TcpSocket::startCoroutines() {
    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->receiveCoroutine();
        co_return;
    }, boost::asio::detached);

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->writerCoroutine();
        co_return;
    }, boost::asio::detached);
}

void TcpSocket::closeEvent() {
    asyncEvents.store(false);
    asioConcurrentQueue.close();
    closeSocket();
}

bool TcpSocket::asyncWrite(std::shared_ptr<WriterData> writerData) {
    if (writerData == nullptr) return false;
    if (!asyncEvents.load()) return false;
    return asioConcurrentQueue.enqueue(std::move(writerData));
}

bool TcpSocket::isOpen() const {
    return asyncEvents.load() && tcpSocket.is_open();
}

void TcpSocket::setOnMessageHandle(std::function<void(std::string)> handle) {
    this->onMessageHandle = std::move(handle);
}

void TcpSocket::setOnDisConnectHandle(std::function<void()> handle) {
    this->onDisConnectHandle = std::move(handle);
}

boost::asio::awaitable<void> TcpSocket::receiveCoroutine() {
    char headerBuffer[8];
    const size_t headerSize = sizeof(int64_t);

    try {
        while (asyncEvents.load()) {

            std::memset(headerBuffer, 0, headerSize);

            size_t headerRead = 0;
            while (headerRead < headerSize) {
                size_t n = co_await tcpSocket.async_read_some(
                    boost::asio::buffer(headerBuffer + headerRead, headerSize - headerRead),
                    boost::asio::use_awaitable);

                if (n == 0) co_return;

                headerRead += n;
            }

            int64_t rawBodyLength = 0;
            std::memcpy(&rawBodyLength, headerBuffer, sizeof(int64_t));
            int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

            if (bodyLength <= 0 || bodyLength > 10 * 1024 * 1024) {
                LOG_ERROR("TcpSocket receiveCoroutine invalid body length: %d", static_cast<int>(bodyLength));
                co_return;
            }

            const size_t bodySize = static_cast<size_t>(bodyLength);

            std::unique_ptr<char[]> bodyBuffer(new char[bodySize + 1]);
            if (!bodyBuffer) {
                LOG_ERROR("TcpSocket receiveCoroutine failed to allocate body buffer");
                co_return;
            }
            std::memset(bodyBuffer.get(), 0, bodySize + 1);

            size_t bodyRead = 0;
            while (bodyRead < bodySize) {
                size_t n = co_await tcpSocket.async_read_some(
                    boost::asio::buffer(bodyBuffer.get() + bodyRead, bodySize - bodyRead),
                    boost::asio::use_awaitable);

                if (n == 0) co_return;

                bodyRead += n;
            }

            std::string bodyStr(bodyBuffer.get(), bodySize);

            if (onMessageHandle) {
                onMessageHandle(std::move(bodyStr));
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("TcpSocket receiveCoroutine error: %s", e.what());
        disconnectEvent();
    }
    catch (...) {
        LOG_ERROR("TcpSocket receiveCoroutine unknown error");
        disconnectEvent();
    }

    co_return;
}

boost::asio::awaitable<void> TcpSocket::writerCoroutine() {
    try {
        while (asyncEvents.load()) {

            std::optional<std::shared_ptr<WriterData>> optional = co_await asioConcurrentQueue.dequeue();

            if (optional.has_value()) {
                std::shared_ptr<WriterData> writeData = optional.value();

                co_await boost::asio::async_write(
                    tcpSocket,
                    boost::asio::buffer(writeData->data, writeData->size),
                    boost::asio::use_awaitable);
            }
            else break;

            if (!asyncEvents.load()) break;
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("TcpSocket writerCoroutine error: %s", e.what());
        disconnectEvent();
    }
    catch (...) {
        LOG_ERROR("TcpSocket writerCoroutine unknown error");
        disconnectEvent();
    }

    co_return;
}

void TcpSocket::disconnectEvent() {
    if (!asyncEvents.exchange(false)) return;

    asioConcurrentQueue.close();

    closeSocket();

    if (onDisConnectHandle) {
        onDisConnectHandle();
    }
}

void TcpSocket::closeSocket() {
    boost::system::error_code errorCode;

    tcpSocket.cancel(errorCode);
    if (errorCode) {
        LOG_WARN("TcpSocket::closeSocket cancel failed: %s", errorCode.message().c_str());
    }

    if (tcpSocket.is_open()) {
        tcpSocket.close(errorCode);
        if (errorCode && errorCode != boost::asio::error::not_connected) {
            LOG_ERROR("TcpSocket::closeSocket close failed: %s", errorCode.message().c_str());
        }
    }
}

void TcpSocket::setTcpKeepAlive(boost::asio::ip::tcp::socket& socket, int idle, int interval, int probes)
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
