#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "AsioProactors.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "HttpSocket.h"
#include "WebRTCMysqlManagerPools.h"
#include "ConfigManager.h"
#include "Utils.h"

namespace hope {

    namespace core {

        WebRTCSignalServer::WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port, int enableHttp, size_t httpPort, size_t enablePublicPort, size_t size)
            : ioContext(ioContext)
            , port(port)
            , enableHttp(enableHttp)
            , httpPort(httpPort)
            , enablePublicPort(enablePublicPort)
#ifndef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif
            , size(size)
            , webrtcSignalManagers(size)
            , taskQueues(ioContext, ConfigManager::Instance().GetInt("WebRTCSignalServer.overload")* (size + 1))
        {

#ifndef __linux__

            boost::asio::ip::address address = enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            try {

                acceptor.open(boost::asio::ip::tcp::v4());
                acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                acceptor.bind(boost::asio::ip::tcp::endpoint(address, port));
                acceptor.listen(boost::asio::socket_base::max_listen_connections);

                httpAcceptor.open(boost::asio::ip::tcp::v4());
                httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                httpAcceptor.bind(boost::asio::ip::tcp::endpoint(address, httpPort));
                httpAcceptor.listen(boost::asio::socket_base::max_listen_connections);

            }
            catch (const std::exception& e) {
                LOG_ERROR("Acceptor setup failed: %s", e.what());
                throw;
            }

#endif

            initialize();

        }

        void WebRTCSignalServer::asyncEvent() {

            if (asyncEvents.exchange(true)) return;

            LOG_INFO("WebRTCSginalServer Protocol: WebSocket , Listen Accept Port: %zu", port);

            if (enableHttp == 1) {

                LOG_INFO("WebRTCSginalServer Protocol: Https , Listen Accept Port: %zu", httpPort);

            }

#ifndef __linux__

            boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::shared_ptr<WebRTCSignalManager> manager = loadBalanceWebrtcManger();

                    std::shared_ptr<hope::core::WebRTCSignalSocket> webrtcSignalSocket = manager->generateWebRTCSignalSocket();

                    co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = manager->shared_from_this()](std::string accountId, std::string sessionId) {

                        sharedManager->removeConnection(accountId, sessionId);

                        });

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [webrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        if (co_await webrtcSignalSocket->handShake()) {

                            webrtcSignalSocket->asyncEvent();

                        }

                        }, boost::asio::detached);


                }

                }, boost::asio::detached);

            if (enableHttp == 1) {

                boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                    while (asyncEvents.load()) {

                        std::shared_ptr<WebRTCSignalManager> manager = loadBalanceWebrtcManger();

                        std::shared_ptr<HttpSocket> httpSocket = manager->generateHttpSocket(true);

                        co_await httpAcceptor.async_accept(httpSocket->getSocket(), boost::asio::use_awaitable);

                        boost::asio::co_spawn(httpSocket->getIoContext(), [this, httpSocket = httpSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                            co_await httpSocket->asyncEventLoop();

                            co_return;

                            }, boost::asio::detached);

                    }

                    co_return;

                    }, boost::asio::detached);

            }

#elif defined(__linux__)

            boost::asio::ip::address address = enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            for (int i = 0; i < size; i++) {

                webrtcSignalManagers[i]->asyncAccept(asyncEvents, boost::asio::ip::tcp::endpoint(address, port), boost::asio::ip::tcp::endpoint(address, httpPort), enableHttp);

            }

#endif

            boost::asio::co_spawn(ioContext, [this]()mutable->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::optional<AwaitableTask> optional = co_await taskQueues.dequeue();

                    if (!optional.has_value()) break;

                    AwaitableTask func = std::move(optional.value());

                    if (func) {

                        co_await func();

                    }

                    if (!asyncEvents.load()) break;

                }

                LOG_INFO("WebRTCSignalServer asyncTaskExecute closeAsyncEvent");

                co_return;

                }, boost::asio::detached);

            for (int i = 0; i < size; i++) {

                webrtcSignalManagers[i]->getLogicSystem()->asyncTaskExecute();

            }

            return;

        }


        WebRTCSignalServer::~WebRTCSignalServer() {

            closeEvent();

        }

        void WebRTCSignalServer::closeEvent() {

            if (!closeEvents.exchange(false)) return;

            LOG_INFO("WebRTCSignalServer CloseEvent...");

            taskQueues.close();
            // 设置关闭标志，防止新连接
            webrtcSignalManagers.clear();

            LOG_INFO("WebRTCSignalServer Already CloseEvent");

        }

        bool WebRTCSignalServer::postTaskAsync(size_t channelIndex, std::function <boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager>) > asyncHandle)
        {
            if (channelIndex >= webrtcSignalManagers.size()) {
                LOG_ERROR("Invalid channelIndex: %zu, size: %zu", channelIndex, webrtcSignalManagers.size());
                return false;
            }

            auto manager = webrtcSignalManagers[channelIndex];
            if (!manager) {
                LOG_ERROR("WebRTCSignalManager at index %zu is null", channelIndex);
                return false;
            }

            boost::asio::co_spawn(manager->getLogicSystem()->getIoCompletePorts(),
                [manager = manager->shared_from_this(), asyncHandle = std::move(asyncHandle)]() -> boost::asio::awaitable<void> {
                    co_await asyncHandle(manager);
                }, [this](std::exception_ptr ptr) {
                    if (ptr) {
                        try {
                            std::rethrow_exception(ptr);
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("WebRTCSignalServer boost::asio::co_spawn Exception: %s", e.what());
                        }
                    }
                    });

                return true;
        }


        std::shared_ptr<WebRTCSignalManager> WebRTCSignalServer::loadBalanceWebrtcManger()
        {
            size_t index = managerIndex.fetch_add(1) % size;

            return webrtcSignalManagers[index];

        }

        size_t WebRTCSignalServer::getChannelNumbers()
        {
            return webrtcSignalManagers.size();
        }

        void WebRTCSignalServer::initialize()
        {

            for (int i = 0; i < size; i++) {

                boost::asio::io_context& ioContext = hope::iocp::AsioProactors::getInstance()->getIoCompletePort(i);

                webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(i, ioContext, this, taskQueues);

            }

        }
    }

}