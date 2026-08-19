#include "TcpAcceptor.h"

#include "../utils/Utils.h"

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
    }, boost::asio::detached);
}

void TcpAcceptor::stopAccept() {
    if (!acceptRunning.exchange(false)) return;

    boost::system::error_code errorCode;

    acceptor.cancel(errorCode);
    if (errorCode) {
        LOG_WARN("TcpAcceptor::stopAccept cancel failed: %s", errorCode.message().c_str());
    }

    acceptor.close(errorCode);
    if (errorCode) {
        LOG_WARN("TcpAcceptor::stopAccept close failed: %s", errorCode.message().c_str());
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
            LOG_WARN("TcpAcceptor accept loop stopped: %s", e.what());
            co_return;
        }
        catch (...) {
            acceptRunning.store(false);
            LOG_WARN("TcpAcceptor accept loop stopped: unknown error");
            co_return;
        }

        tcpSocket->asioConcurrentQueue.reset();
        tcpSocket->setTcpKeepAlive(tcpSocket->tcpSocket);
        tcpSocket->asyncEvents.store(true);
        tcpSocket->startCoroutines();

        if (currentTcpSocket) {
            currentTcpSocket->closeEvent();
        }

        currentTcpSocket = tcpSocket;

        LOG_INFO("TcpAcceptor accepted a new tcpSocket");

        if (onAcceptHandle) {
            onAcceptHandle(tcpSocket);
        }
    }

    co_return;
}

}

}
