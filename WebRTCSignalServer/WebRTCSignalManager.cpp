#include "WebRTCSignalManager.h"
#include "WebRTCSignalServer.h"

#include "WebRTCSignalData.h"

#include "Utils.h"

namespace hope {
	
	namespace core {
        WebRTCSignalManager::WebRTCSignalManager(boost::asio::io_context& ioContext, int channelIndex, WebRTCSignalServer* webrtcSignalServer) : ioContext(ioContext)
            , channelIndex(channelIndex)
            , webrtcSignalServer(webrtcSignalServer)
            , localRouteCache([](std::string) -> int {
            return -1;
                }, 100)
            , webrtcLogicSystem(nullptr)
        {
            webrtcLogicSystem = std::make_shared<WebRTCLogicSystem>();

            webrtcLogicSystem->RunEventLoop();

        }

        WebRTCSignalManager::~WebRTCSignalManager()
        {
            webrtcSignalSocketMap.clear();
        }



        std::shared_ptr<WebRTCSignalSocket> WebRTCSignalManager::generateWebRTCSignalSocket()
        {

            std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<WebRTCSignalSocket>(ioContext, channelIndex, this);

            webrtcSignalSocket->setOnDisConnectHandle([self = shared_from_this()](std::string accountID) {
                self->removeConnection(accountID); // 清理连接映射
                });

            return webrtcSignalSocket;
        }

        boost::asio::io_context& WebRTCSignalManager::getIoCompletePorts()
        {
            return ioContext;
        }


        void WebRTCSignalManager::removeConnection(const std::string& accountID)
        {
            LOG_INFO("Remove WebRTCSignalSocket: %s", accountID.c_str());

            std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

            {
                // 使用互斥锁或原子操作保护
                auto it = webrtcSignalSocketMap.find(accountID);

                if (it == webrtcSignalSocketMap.end()) {

                    LOG_WARNING("Connection already removed: %s", accountID.c_str());

                    return;
                }
                webrtcSignalSocket = it->second;

                webrtcSignalSocketMap.erase(it);

            }

            if (!webrtcSignalSocket) {

                return;

            }

            webrtcSignalSocket = nullptr;

            int mapChannelIndex = hasher(accountID) % hashSize;

            LOG_INFO("Start Async Post Task: %d", mapChannelIndex);

            webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = shared_from_this(), accountID](std::shared_ptr<WebRTCSignalManager> manager) -> boost::asio::awaitable<void> {

                manager->getActorSocketMappingIndex().erase(accountID);

                co_return;

                });
            

        }

        hope::utils::WebRTCHashMap<std::string, int>& WebRTCSignalManager::getActorSocketMappingIndex()
        {
            return actorSocketMappingIndex;
        }

        std::shared_ptr<WebRTCLogicSystem> WebRTCSignalManager::getWebRTCLogicSystem()
        {
            return webrtcLogicSystem;
        }

	}

}
