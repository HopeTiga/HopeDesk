#include "WebRTCSignalManager.h"
#include "WebRTCSignalServer.h"

namespace Hope {
	
    WebRTCSignalManager::WebRTCSignalManager(boost::asio::io_context& ioContext, int channelIndex, WebRTCSignalServer* webrtcSignalServer) : ioContext(ioContext)
        , channelIndex(channelIndex)
        , webrtcSignalServer(webrtcSignalServer)
        , localRouteCache([](std::string) -> int {
        return -1;
            }, 100)
        , webrtcLogicSystem(nullptr)
        , webrtcMysqlManager(nullptr)
	{
        webrtcLogicSystem = std::make_shared<WebRTCLogicSystem>();

		webrtcLogicSystem->RunEventLoop();

		webrtcMysqlManager = std::make_unique<WebRTCMysqlManager>(ioContext);

		webrtcMysqlManager->initConnection("sh-cynosdbmysql-grp-75t233bm.sql.tencentcdb.com", 22008, "CloudGame", "WebRTC251028~Dd", "cloudgame");
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
		LOG_INFO("移除连接: %s", accountID.c_str());

        webrtcSignalSocketMap[accountID]->stop();

        webrtcSignalSocketMap.unsafe_erase(accountID);

        int mapChannelIndex = hasher(accountID) % hashSize;

        LOG_INFO("开始异步回调: %d", mapChannelIndex);

        webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = shared_from_this(), accountID](std::shared_ptr<WebRTCSignalManager> manager) {

            manager->getActorSocketMappingIndex().unsafe_erase(accountID);

            });

    }

    tbb::concurrent_unordered_map<std::string, int>& WebRTCSignalManager::getActorSocketMappingIndex()
    {
		return actorSocketMappingIndex;
    }

    std::shared_ptr<WebRTCLogicSystem> WebRTCSignalManager::getWebRTCLogicSystem()
    {
        return webrtcLogicSystem;
    }

	

}
