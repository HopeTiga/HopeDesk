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
#include "Utils.h"

namespace hope {

    namespace core {
        WebRTCSignalServer::WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port, int enableHttp, size_t httpPort, size_t size)
            : ioContext(ioContext)
            , port(port)
            , enableHttp(enableHttp)
            , httpPort(httpPort)
#ifndef __linux__
            , acceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), port))
            , httpAcceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), httpPort))
#endif
            , size(size)
            , webrtcSignalManagers(size)
            , taskQueues(ioContext)
        {
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

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [this, selfWebRTCSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        co_await selfWebRTCSignalSocket->handShake();

                        selfWebRTCSignalSocket->asyncEvent();

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

            for (int i = 0; i < size; i++) {

                webrtcSignalManagers[i]->asyncAccept(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), port), asyncEvents, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), httpPort), enableHttp);

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

            if (closeEvents.exchange(true)) return;

            asyncEvents.store(false);

            LOG_INFO("WebRTCSignalServer Start Shutdown...");

            taskQueues.close();
            // 设置关闭标志，防止新连接
            webrtcSignalManagers.clear();

            LOG_INFO("WebRTCSignalServer Already Shutdown");

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

                std::pair<int, boost::asio::io_context&> channelPairs = hope::iocp::AsioProactors::getInstance()->getIoCompletePorts();

                webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(channelPairs.first, channelPairs.second, this, taskQueues);

            }

        }
    }

}