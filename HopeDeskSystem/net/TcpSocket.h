#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "AsioConcurrentQueue.h"
#include "WriterData.h"

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
    void startCoroutines();

    boost::asio::awaitable<void> receiveCoroutine();

    boost::asio::awaitable<void> writerCoroutine();

    void disconnectEvent();

    void closeSocket();

    void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket, int idle = 30, int interval = 30, int probes = 30);

    boost::asio::io_context& ioContext;

    boost::asio::ip::tcp::socket tcpSocket;

    AsioConcurrentQueue<std::shared_ptr<WriterData>> asioConcurrentQueue;

    std::atomic<bool> asyncEvents{ false };

    std::atomic<bool> connecting{ false };

    std::function<void(std::string)> onMessageHandle;

    std::function<void()> onDisConnectHandle;
};

}

}
