#include "TcpAcceptor.h"

#include "../utils/Utils.h"
#include "../utils/CompletionHandle.h"

namespace hope {

namespace net {

TcpAcceptor::TcpAcceptor(boost::asio::io_context& ioContext, unsigned short port)
    : ioContext(ioContext)
    , acceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address_v4("127.0.0.1"), port)) {
}

TcpAcceptor::~TcpAcceptor() {
    stopAccept();
}

void TcpAcceptor::startAccept() {
    if (acceptRunning.exchange(true)) return;

    boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->acceptCoroutine();
        co_return;
    }, hope::CompletionHandle{});
}

void TcpAcceptor::stopAccept() {
    if (!acceptRunning.exchange(false)) return;

    boost::system::error_code errorCode;

    acceptor.cancel(errorCode);
    if (errorCode) {
        LOG_WARN("TcpAcceptor::StopAccept Cancel Failed: {}", errorCode.message().c_str());
    }

    acceptor.close(errorCode);
    if (errorCode) {
        LOG_WARN("TcpAcceptor::StopAccept Close Failed: {}", errorCode.message().c_str());
    }
}

void TcpAcceptor::setOnAcceptHandle(std::function<void(std::shared_ptr<TcpSocket>)> handle) {
    this->onAcceptHandle = std::move(handle);
}

boost::asio::awaitable<void> TcpAcceptor::acceptCoroutine() {
    while (acceptRunning.load()) {

        std::shared_ptr<TcpSocket> tcpSocket = std::make_shared<TcpSocket>(ioContext);

        try {
            co_await acceptor.async_accept(tcpSocket->tcpSocket, boost::asio::use_awaitable);
        }
        catch (const std::exception& e) {
            acceptRunning.store(false);
            LOG_WARN("TcpAcceptor Accept Loop Stopped: {}", e.what());
            co_return;
        }
        catch (...) {
            acceptRunning.store(false);
            LOG_WARN("TcpAcceptor Accept Loop Stopped: Unknown Error");
            co_return;
        }

        tcpSocket->asioConcurrentQueue.reset();
        tcpSocket->setTcpKeepAlive(tcpSocket->tcpSocket);
        tcpSocket->asyncBoots.store(true);
        tcpSocket->startCoroutines();

        if (currentTcpSocket) {
            currentTcpSocket->closeEvent();
        }

        currentTcpSocket = tcpSocket;

        LOG_INFO("TcpAcceptor Accepted A New TcpSocket");

        if (onAcceptHandle) {
            onAcceptHandle(tcpSocket);
        }
    }

    co_return;
}

}

}
