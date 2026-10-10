#include "TcpSocket.h"

#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "../utils/Utils.h"
#include "../utils/CompletionHandle.h"

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
    , awaitableQueue(ioContext.get_executor()) {
    receiveBuffer.resize(receiveBufferInitialSize);
}

TcpSocket::~TcpSocket() {
    closeEvent();
}

boost::asio::awaitable<bool> TcpSocket::connect(unsigned short port) {
    if (connecting.exchange(true)) {
        LOG_WARN("TcpSocket::Connect: Already Connecting, Skip Duplicate Connect");
        co_return false;
    }

    try {

        boost::asio::ip::address address = boost::asio::ip::make_address("127.0.0.1");

        boost::asio::ip::tcp::endpoint endpoint(address, port);

        co_await boost::asio::async_connect(tcpSocket, std::array{ endpoint }, boost::asio::deferred);

        awaitableQueue.reset();

        setTcpKeepAlive(tcpSocket);

        connecting.store(false);

        asyncEvent();

        LOG_INFO("TcpSocket Connected To 127.0.0.1:{}", static_cast<unsigned int>(port));

        co_return true;
    }
    catch (const std::exception& e) {
        connecting.store(false);
        LOG_ERROR("TcpSocket::Connect Error: {}", e.what());
        closeSocket();
        co_return false;
    }
    catch (...) {
        connecting.store(false);
        LOG_ERROR("TcpSocket::Connect Unknown Error");
        closeSocket();
        co_return false;
    }
}

void TcpSocket::asyncEvent() {
    if (asyncEvents.exchange(true)) return;

    isHandleDisConnect.store(false);

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->receiveCoroutine();
        co_return;
    }, [self = shared_from_this()](std::exception_ptr error) {
        self->handleCoroutineExit("ReceiveCoroutine", error);
    });

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->writerCoroutine();
        co_return;
    }, [self = shared_from_this()](std::exception_ptr error) {
        self->handleCoroutineExit("WriterCoroutine", error);
    });
}

void TcpSocket::closeEvent() {
    if (!asyncEvents.exchange(false)) return;
    awaitableQueue.close();
    closeSocket();
}

bool TcpSocket::asyncWrite(std::shared_ptr<WriterData> writerData) {
    if (writerData == nullptr) return false;
    if (!asyncEvents.load()) return false;
    return awaitableQueue.enqueue(std::move(writerData));
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
    while (asyncEvents.load()) {

        if (receiveHeldBytes == receiveBuffer.size()) {

            if (receiveBuffer.size() >= receiveBufferMaximumSize) {
                throw std::runtime_error("Frame Larger Than The Maximum Receive Buffer");
            }

            std::size_t grownSize = receiveBuffer.size() * 2;

            if (grownSize > receiveBufferMaximumSize) grownSize = receiveBufferMaximumSize;

            receiveBuffer.resize(grownSize);
        }

        const std::size_t receivedBytes = co_await tcpSocket.async_read_some(
            boost::asio::buffer(receiveBuffer.data() + receiveHeldBytes,
                                receiveBuffer.size() - receiveHeldBytes),
            boost::asio::deferred);

        if (receivedBytes == 0) co_return;

        const std::size_t totalBytes = receiveHeldBytes + receivedBytes;

        std::size_t offset = 0;

        while (totalBytes - offset >= headerSize) {

            int64_t rawBodyLength = 0;
            std::memcpy(&rawBodyLength, receiveBuffer.data() + offset, sizeof(int64_t));

            int64_t bodyLength = boost::asio::detail::socket_ops::network_to_host_long(rawBodyLength);

            if (bodyLength <= 0 || bodyLength > static_cast<int64_t>(maximumBodySize)) {
                LOG_ERROR("TcpSocket ReceiveCoroutine Invalid Body Length: {}", static_cast<int>(bodyLength));
                throw std::runtime_error("Invalid Body Length");
            }

            const std::size_t bodySize = static_cast<std::size_t>(bodyLength);

            if (totalBytes - offset - headerSize < bodySize) break;

            std::string bodyStr(receiveBuffer.data() + offset + headerSize, bodySize);

            offset += headerSize + bodySize;

            if (onMessageHandle) {
                onMessageHandle(std::move(bodyStr));
            }
        }

        receiveHeldBytes = totalBytes - offset;

        if (receiveHeldBytes > 0 && offset > 0) {
            std::memmove(receiveBuffer.data(), receiveBuffer.data() + offset, receiveHeldBytes);
        }
    }

    co_return;
}

boost::asio::awaitable<void> TcpSocket::writerCoroutine() {

    std::vector<std::shared_ptr<WriterData>> packets(maximumFramesPerWrite);

    std::vector<boost::asio::const_buffer> segments;

    segments.reserve(maximumFramesPerWrite);

    while (asyncEvents.load()) {

        std::size_t count = awaitableQueue.tryDequeueBulk(packets.data(), maximumFramesPerWrite);

        if (count == 0) {

            if (!co_await awaitableQueue.awaitDequeue(packets[0])) break;

            count = 1;
        }

        segments.clear();

        for (std::size_t index = 0; index < count; ++index) {
            segments.emplace_back(boost::asio::buffer(packets[index]->data, packets[index]->size));
        }

        co_await boost::asio::async_write(tcpSocket, segments, boost::asio::deferred);
    }

    co_return;
}

void TcpSocket::handleCoroutineExit(std::string_view coroutineName, std::exception_ptr error) {
    if (!asyncEvents.load(std::memory_order_acquire)) return;

    closeEvent();

    if (onDisConnectHandle && !isHandleDisConnect.exchange(true)) {
        onDisConnectHandle();
    }

    if (!error) {
        LOG_INFO("{} Exit: Peer Closed The Connection", coroutineName);
        return;
    }

    try {
        std::rethrow_exception(error);
    }
    catch (const std::exception& e) {
        LOG_ERROR("{} Error: {}", coroutineName, e.what());
    }
    catch (...) {
        LOG_ERROR("{} Error: Unknown", coroutineName);
    }
}

void TcpSocket::closeSocket() {
    boost::system::error_code errorCode;

    tcpSocket.cancel(errorCode);
    if (errorCode) {
        LOG_WARN("TcpSocket::CloseSocket Cancel Failed: {}", errorCode.message().c_str());
    }

    if (tcpSocket.is_open()) {
        tcpSocket.close(errorCode);
        if (errorCode && errorCode != boost::asio::error::not_connected) {
            LOG_ERROR("TcpSocket::CloseSocket Close Failed: {}", errorCode.message().c_str());
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
