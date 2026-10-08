#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio.hpp>

#include "AwaitableQueue.h"
#include "Socket.h"

namespace hope {

namespace net {

class TcpSocket : public std::enable_shared_from_this<TcpSocket> {
public:
    explicit TcpSocket(boost::asio::io_context& ioContext);

    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;

    TcpSocket& operator=(const TcpSocket&) = delete;

    boost::asio::awaitable<bool> connect(unsigned short port);

    void closeEvent();

    bool asyncWrite(std::shared_ptr<WriterData> writerData);

    bool isOpen() const;

    void setOnMessageHandle(std::function<void(std::string)> handle);

    void setOnDisConnectHandle(std::function<void()> handle);

private:
    void asyncEvent();

    boost::asio::awaitable<void> receiveCoroutine();

    boost::asio::awaitable<void> writerCoroutine();

    void handleCoroutineExit(std::string_view coroutineName, std::exception_ptr error);

    void closeSocket();

    void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket, int idle = 0, int interval = 10, int probes = 10);

    boost::asio::io_context& ioContext;

    boost::asio::ip::tcp::socket tcpSocket;

    AwaitableQueue<std::shared_ptr<WriterData>> awaitableQueue;

    std::atomic<bool> asyncEvents{ false };

    std::atomic<bool> connecting{ false };

    std::atomic<bool> isHandleDisConnect{ false };

    std::function<void(std::string)> onMessageHandle;

    std::function<void()> onDisConnectHandle;

    std::vector<char> receiveBuffer;

    std::size_t receiveHeldBytes{ 0 };

    static constexpr std::size_t headerSize{ sizeof(int64_t) };

    static constexpr std::size_t maximumBodySize{ 10 * 1024 * 1024 };

    static constexpr std::size_t receiveBufferInitialSize{ 8192 };

    static constexpr std::size_t receiveBufferMaximumSize{ maximumBodySize + headerSize };

    static constexpr std::size_t maximumFramesPerWrite{ 32 };
};

}

}
