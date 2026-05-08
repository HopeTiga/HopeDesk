#include "WebRTCSignalManager.h"

#include <boost/asio.hpp>

#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"

#include "WebRTCSignalData.h"

#include "AsioProactors.h"

#include "Utils.h"

namespace hope {

    namespace core {

        WebRTCSignalManager::WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer * webrtcSignalServer)
            : channelIndex(channelIndex)
            , ioContext(ioContext)
            , webrtcSignalServer(webrtcSignalServer)
#ifdef __linux__
            , acceptor(ioContext)
#endif

        {

            logicSystem = std::make_shared<hope::core::WebRTCLogicSystem>(hope::iocp::AsioProactors::getLogicInstance()->getIoCompletePorts().second,channelIndex);

            logicSystem->initHandlers();

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


        void WebRTCSignalManager::removeConnection(std::string accountId, std::string sessionId, bool needClear)
        {
            LOG_INFO("Remove WebRTCSignalSocketInterface Request: Account=%s, SessionId=%s", accountId.c_str(), sessionId.c_str());

            auto it = webrtcSocketMap.find(accountId);

            if (it == webrtcSocketMap.end()) {
                LOG_WARNING("Connection already removed or not found: %s", accountId.c_str());
                return;
            }

            std::shared_ptr<WebRTCSignalSocket> currentSocket = it->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARNING("Race Condition Detected! Ignore remove request. "
                    "Account: %s, RequestSessionId: %s, CurrentMapSessionId: %s",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return;
            }

            webrtcSocketMap.erase(it);

            currentSocket->closeSocket();

            if (needClear) {

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


        }

        int WebRTCSignalManager::getChannelIndex()
        {
            return channelIndex;
        }

#ifdef __linux__

        void WebRTCSignalManager::asyncAccept(boost::asio::ip::tcp::endpoint endpoint,std::atomic<bool>& runAccepct)
        {
           
            acceptor.open(endpoint.protocol());

            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            acceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

            acceptor.bind(endpoint);

            acceptor.listen();

            boost::asio::co_spawn(ioContext, [self = shared_from_this()]() ->boost::asio::awaitable<void> {

                while (runAccepct.load()) {

                    std::shared_ptr<hope::core::WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<hope::core::WebRTCSignalSocket>(self->ioContext, self.get());

                    co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = self->shared_from_this()](std::string accountId, std::string sessionId) {

                        sharedManager->removeConnection(accountId, sessionId);

                        });

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [this, selfWebRTCSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        co_await selfWebRTCSignalSocket->handShake();

                        selfWebRTCSignalSocket->runEventLoop();

                        }, boost::asio::detached);


                }

                }, boost::asio::detached);
        }

#endif

    }

}