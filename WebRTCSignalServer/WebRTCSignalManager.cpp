#include "WebRTCSignalManager.h"

#include <boost/asio.hpp>

#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"

#include "WebRTCSignalData.h"

#include "Utils.h"

namespace hope {

    namespace core {

        WebRTCSignalManager::WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer * webrtcSignalServer)
            : channelIndex(channelIndex)
            , ioContext(ioContext)
            , webrtcSignalServer(webrtcSignalServer)
            , localRouteCache([](std::string) -> int {
            return -1;
                }, 100)
        {
            logicSystem = std::make_shared<hope::core::WebRTCLogicSystem>(ioContext);

            logicSystem->RunEventLoop();
        }

        WebRTCSignalManager::~WebRTCSignalManager()
        {
            webrtcSocketMap.clear();

        }

        std::shared_ptr<WebRTCLogicSystem> WebRTCSignalManager::getLogicSystem()
        {
            return logicSystem;
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

    }

}