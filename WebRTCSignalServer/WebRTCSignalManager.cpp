#include "WebRTCSignalManager.h"
#include "WebRTCSignalServer.h"
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
            LOG_INFO("Remove WebRTCSignalSocket: %s", accountID.c_str());

            if (webrtcSignalSocketMap[accountID]->getCloudGame()) {

				auto self = shared_from_this();
                
                boost::asio::co_spawn(this->webrtcLogicSystem->getIoCompletePorts(),[self, accountID]() -> boost::asio::awaitable<void> {

                    std::shared_ptr<boost::mysql::any_connection> connection = self->webrtcMysqlManager->getConnection();

					boost::mysql::results updateResults;

                    boost::mysql::statement updateStmt = connection->prepare_statement(
                        "UPDATE game_processes SET is_idle = 1 , is_login = 0  WHERE process_id = ? and del_flag = 0"
					);

                    co_await connection->async_execute(updateStmt.bind(accountID), updateResults);

                    if (updateResults.affected_rows() == 0) {

                        LOG_ERROR("CloudGameProcessID: %s update state failed!", accountID.c_str());

                        co_return;
                    }

                    LOG_INFO("CloudGameProcessID: %s update state successful!", accountID.c_str());

                    co_return;
                        
                    }, boost::asio::detached);

            }

            webrtcSignalSocketMap.unsafe_erase(accountID);

            int mapChannelIndex = hasher(accountID) % hashSize;

            LOG_INFO("Start Async Post Task: %d", mapChannelIndex);

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

}
