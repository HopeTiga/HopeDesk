#include "WebRTCSignalManager.h"

#include <boost/asio.hpp>

#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"

#include "AsioProactors.h"

#include "Utils.h"

namespace hope {

    namespace core {

        WebRTCSignalManager::WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer* webrtcSignalServer, TaskChannel& taskQueues, size_t size)
            : channelIndex(channelIndex)
            , ioContext(ioContext)
            , webrtcSignalServer(webrtcSignalServer)
            , hashSize(size)
#ifdef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif

        {

            logicSystem = std::make_shared<hope::core::WebRTCLogicSystem>(hope::iocp::AsioProactors::getLogicInstance()->getIoCompletePort(channelIndex), channelIndex, taskQueues);

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

        std::shared_ptr<hope::core::WebRTCSignalSocket> WebRTCSignalManager::generateWebRTCSignalSocket() {

            return std::make_shared<hope::core::WebRTCSignalSocket>(getIoCompletionPorts(), this);

        }

        std::shared_ptr<HttpSocket> WebRTCSignalManager::generateHttpSocket() {

            return std::make_shared<HttpSocket>(ioContext, this);

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

            webrtcSignalServer->postTaskAsync(mapChannelIndex, [accountId](std::shared_ptr<WebRTCSignalManager> manager) -> boost::asio::awaitable<void> {

                auto itIndex = manager->actorSocketMappingIndex.find(accountId);

                if (itIndex != manager->actorSocketMappingIndex.end()) {

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

                    std::shared_ptr<hope::core::WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<hope::core::WebRTCSignalSocket>(self->ioContext, self.get());

                    co_await self->acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = self->shared_from_this()](std::string accountId, std::string sessionId) {

                        boost::asio::io_context & ioContext = sharedManager->ioContext;

                        boost::asio::post(ioContext, [sharedManager = std::move(sharedManager), accountId = std::move(accountId), sessionId = std::move(sessionId)] {
                            
                            sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

                            });

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