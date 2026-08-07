#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <boost/asio.hpp>

#include "TcpSocket.h"

namespace hope {

namespace net {

class TcpAcceptor : public std::enable_shared_from_this<TcpAcceptor> {
public:
    explicit TcpAcceptor(boost::asio::io_context& ioContext, unsigned short port = 19998);

    ~TcpAcceptor();

    TcpAcceptor(const TcpAcceptor&) = delete;

    TcpAcceptor& operator=(const TcpAcceptor&) = delete;

    void startAccept();

    void stopAccept();

    void setOnAcceptHandle(std::function<void(std::shared_ptr<TcpSocket>)> handle);

private:
    boost::asio::awaitable<void> acceptCoroutine();

    boost::asio::io_context& ioContext;

    boost::asio::ip::tcp::acceptor acceptor;

    std::shared_ptr<TcpSocket> currentTcpSocket;

    std::atomic<bool> acceptRunning{ false };

    std::function<void(std::shared_ptr<TcpSocket>)> onAcceptHandle;
};

}

}
