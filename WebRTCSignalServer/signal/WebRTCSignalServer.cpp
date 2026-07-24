#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "../iocp/AsioProactors.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "HttpSocket.h"
#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        WebRTCSignalServer::WebRTCSignalServer(boost::asio::io_context& ioContext, WebRTCSignalConfig webrtcSignalConfig)
            : ioContext(ioContext)
            , webrtcSignalConfig(webrtcSignalConfig)
#ifndef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif
            , webrtcSignalManagers(webrtcSignalConfig.threadSize)
            , coroRpc(nullptr)
            , taskQueues(ioContext, webrtcSignalConfig.overload * (webrtcSignalConfig.threadSize + 1))
        {

#ifndef __linux__

            boost::asio::ip::address address = webrtcSignalConfig.enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            try {

                acceptor.open(boost::asio::ip::tcp::v4());
                acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                acceptor.bind(boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.signalPort));
                acceptor.listen(boost::asio::socket_base::max_listen_connections);

                httpAcceptor.open(boost::asio::ip::tcp::v4());
                httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                httpAcceptor.bind(boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.httpPort));
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

            LOG_INFO("WebRTCSginalServer Protocol: WebSocket , Listen Accept Port: %zu", webrtcSignalConfig.signalPort);

            if (webrtcSignalConfig.enableHttp == 1) {

                LOG_INFO("WebRTCSginalServer Protocol: Https , Listen Accept Port: %zu", webrtcSignalConfig.httpPort);

            }

#ifndef __linux__

            boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::shared_ptr<WebRTCSignalManager> manager = loadBalanceWebrtcManger();

                    std::shared_ptr<hope::signal::WebRTCSignalSocket> webrtcSignalSocket = manager->generateWebRTCSignalSocket();

                    co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = manager->shared_from_this()](std::string accountId, std::string sessionId) {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

                        sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

#else
                        boost::asio::io_context& ioContext = sharedManager->getIoCompletionPorts();

                        boost::asio::post(ioContext, [sharedManager = std::move(sharedManager), accountId = std::move(accountId), sessionId = std::move(sessionId)] {

                            sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

                            });
#endif


                        });

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [webrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        if (co_await webrtcSignalSocket->handShake()) {

                            webrtcSignalSocket->asyncEvent();

                        }

                        }, boost::asio::detached);


                }

                }, boost::asio::detached);

            if (webrtcSignalConfig.enableHttp == 1) {

                boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                    while (asyncEvents.load()) {

                        std::shared_ptr<WebRTCSignalManager> manager = loadBalanceWebrtcManger();

                        std::shared_ptr<HttpSocket> httpSocket = manager->generateHttpSocket();

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

            boost::asio::ip::address address = webrtcSignalConfig.enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->asyncAccept(asyncEvents, boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.signalPort), boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.httpPort), static_cast<int>(webrtcSignalConfig.enableHttp));

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

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->getLogicSystem()->asyncTaskExecute();

            }

            if (webrtcSignalConfig.enableRpc == 1) {
            
                coroRpc = std::make_shared<hope::rpc::CoroRpc>(webrtcSignalConfig.coroRpcServerConfig);

                coroRpc->asyncEvent();

                LOG_INFO("WebRTCSginalServer Protocol: CoroRpc , Listen Accept Port: %zu", webrtcSignalConfig.coroRpcServerConfig.port);

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
      
            webrtcSignalManagers.clear();

            LOG_INFO("WebRTCSignalServer Already CloseEvent");

        }

        bool WebRTCSignalServer::postTaskAsync(size_t channelIndex, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager>)>&& asyncHandle)
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

            boost::asio::co_spawn(manager->getLogicSystem()->getIoCompletionPorts(),
                [manager = manager->shared_from_this(), asyncHandle = std::move(asyncHandle)]()mutable -> boost::asio::awaitable<void> {
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
            size_t index = managerIndex.fetch_add(1) % webrtcSignalConfig.threadSize;

            return webrtcSignalManagers[index];

        }

        size_t WebRTCSignalServer::getChannelNumbers()
        {
            return webrtcSignalManagers.size();
        }

        void WebRTCSignalServer::initialize()
        {

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                boost::asio::io_context& ioContext = hope::iocp::AsioProactors::getInstance()->getIoCompletePort(i);

                WebRTCSignalChannelConfig channelConfig{
                    webrtcSignalConfig.threadSize,
                    webrtcSignalConfig.threshold,
                    webrtcSignalConfig.exitThreshold,
                    webrtcSignalConfig.asyncThreshold,
                    webrtcSignalConfig.socketWaitTime
                };

                webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(i, ioContext, this, taskQueues, channelConfig);

            }

        }
    }

}