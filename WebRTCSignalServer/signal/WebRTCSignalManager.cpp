#include "WebRTCSignalManager.h"

#include <boost/asio.hpp>

#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"

#include "../iocp/AsioProactors.h"

#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        WebRTCSignalManager::WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer* webrtcSignalServer, TaskChannel& taskQueues, WebRTCSignalChannelConfig channelConfig)
            : channelIndex(channelIndex)
            , ioContext(ioContext)
            , webrtcSignalServer(webrtcSignalServer)
            , hashSize(channelConfig.hashSize)
            , channelConfig(channelConfig)
#ifdef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif

        {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

            logicSystem = std::make_shared<hope::signal::WebRTCLogicSystem>(ioContext, channelIndex, taskQueues, channelConfig.threshold, channelConfig.exitThreshold, channelConfig.asyncThreshold);

#else

            logicSystem = std::make_shared<hope::signal::WebRTCLogicSystem>(hope::iocp::AsioProactors::getLogicInstance()->getIoCompletePort(channelIndex), channelIndex, taskQueues, channelConfig.threshold, channelConfig.exitThreshold, channelConfig.asyncThreshold);

#endif

            logicSystem->asyncEvent();

        }

        WebRTCSignalManager::~WebRTCSignalManager()
        {
            webrtcSocketMap.clear();

        }

        std::shared_ptr<WebRTCLogicSystem> WebRTCSignalManager::getLogicSystem()
        {
            return logicSystem;
        }

        boost::asio::io_context& WebRTCSignalManager::getIoCompletionPorts() {

            return ioContext;

        }

        std::shared_ptr<hope::signal::WebRTCSignalSocket> WebRTCSignalManager::generateWebRTCSignalSocket() {

            return std::make_shared<hope::signal::WebRTCSignalSocket>(getIoCompletionPorts(), this, channelConfig.socketWaitTime);

        }

        std::shared_ptr<HttpSocket> WebRTCSignalManager::generateHttpSocket() {

            return std::make_shared<HttpSocket>(ioContext, this);

        }

        void WebRTCSignalManager::registerSocket(const std::string& accountId, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket) {

            std::string sessionId = webrtcSignalSocket->getSessionId();

            absl::node_hash_map<std::string, std::shared_ptr<WebRTCSignalSocket>>::iterator iterator = webrtcSocketMap.find(accountId);

            if (iterator != webrtcSocketMap.end()) {
            
                iterator->second->closeEvent();

            }

            webrtcSocketMap[accountId] = std::move(webrtcSignalSocket);

            int mapChannelIndex = hasher(accountId) % hashSize;

            webrtcSignalServer->postTaskAsync(mapChannelIndex,
                [managers = shared_from_this(), accountId, sessionId = std::move(sessionId), mapChannelIndex](std::shared_ptr<WebRTCSignalManager> webrtcSignalManager) -> boost::asio::awaitable<void> {

                    WebRTCSignalManager::ActorMapping actorMapping{ std::move(sessionId), static_cast<int>(managers->channelIndex)};

                    webrtcSignalManager->actorSocketMappingIndex[accountId] = std::move(actorMapping);

                    co_return;

                });

        }

        void WebRTCSignalManager::removeConnection(std::string accountId, std::string sessionId)
        {
            LOG_INFO("Remove WebRTCSignalSocketInterface Request: Account=%s, SessionId=%s", accountId.c_str(), sessionId.c_str());

            auto it = webrtcSocketMap.find(accountId);

            if (it == webrtcSocketMap.end()) {
                LOG_WARN("Connection already removed or not found: %s", accountId.c_str());
                return;
            }

            std::shared_ptr<WebRTCSignalSocket> currentSocket = it->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARN("Race Condition Detected! Ignore remove request. "
                    "Account: %s, RequestSessionId: %s, CurrentMapSessionId: %s",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return;
            }

            webrtcSocketMap.erase(it);

            currentSocket->closeEvent();

            int mapChannelIndex = hasher(accountId) % hashSize;

            webrtcSignalServer->postTaskAsync(mapChannelIndex, [accountId = std::move(accountId), sessionId = std::move(sessionId)](std::shared_ptr<WebRTCSignalManager> manager) -> boost::asio::awaitable<void> {

                auto itIndex = manager->actorSocketMappingIndex.find(accountId);

                if (itIndex != manager->actorSocketMappingIndex.end() && itIndex->second.sessionId == sessionId) {

                    manager->actorSocketMappingIndex.erase(itIndex);

                    LOG_INFO("Global Index Removed: %s", accountId.c_str());

                }

                co_return;

                });


        }

        int WebRTCSignalManager::getChannelIndex()
        {
            return channelIndex;
        }

#ifdef __linux__

        void WebRTCSignalManager::asyncAccept(std::atomic<bool>& runAccepct, boost::asio::ip::tcp::endpoint endpoint, boost::asio::ip::tcp::endpoint httpEndpoint, int enableHttp)
        {

            acceptor.open(endpoint.protocol());

            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            acceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

            acceptor.bind(endpoint);

            acceptor.listen();

            boost::asio::co_spawn(ioContext, [self = shared_from_this(), &runAccepct]() ->boost::asio::awaitable<void> {

                while (runAccepct.load()) {

                    std::shared_ptr<hope::signal::WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<hope::signal::WebRTCSignalSocket>(self->ioContext, self.get(), self->channelConfig.socketWaitTime);

                    co_await self->acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = self->shared_from_this()](std::string accountId, std::string sessionId) {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

                        sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

#else

                        boost::asio::io_context& ioContext = logicSystem->getIoCompletePort();

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

            if (enableHttp == 1) {

                httpAcceptor.open(httpEndpoint.protocol());

                httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

                httpAcceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

                httpAcceptor.bind(httpEndpoint);

                httpAcceptor.listen();

                boost::asio::co_spawn(ioContext, [self = shared_from_this(), &runAccepct]() ->boost::asio::awaitable<void> {

                    while (runAccepct.load()) {

                        std::shared_ptr<HttpSocket> httpSocket = self->generateHttpSocket();

                        co_await self->httpAcceptor.async_accept(httpSocket->getSocket(), boost::asio::use_awaitable);

                        boost::asio::co_spawn(httpSocket->getIoContext(), [httpSocket = httpSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                            co_await httpSocket->asyncEventLoop();

                            co_return;

                            }, boost::asio::detached);

                    }

                    co_return;

                    }, boost::asio::detached);

            }

        }

#endif

    }

}