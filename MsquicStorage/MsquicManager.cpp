#include "MsquicManager.h"

#include <boost/asio.hpp>

#include "MsquicSignalServer.h"
#include "MsquicSocket.h"

#include "MsquicData.h"

#include "Utils.h"

namespace hope {

	namespace quic {
	
		MsquicManager::MsquicManager(size_t channelIndex, boost::asio::io_context& ioContext, MsquicSignalServer* msquicSignalServer)
			: channelIndex(channelIndex)
			, ioContext(ioContext)
			, msquicSignalServer(msquicSignalServer)
			, localRouteCache([](std::string) -> int {
			return -1;
				}, 100)
		{
			logicSystem = std::make_shared<hope::handle::MsquicLogicSystem>(ioContext);

			logicSystem->RunEventLoop();
		}

		MsquicManager::~MsquicManager()
		{

			msquicSocketMap.clear();

		}

		std::shared_ptr<hope::handle::MsquicLogicSystem> MsquicManager::getMsquicLogicSystem()
		{
			return logicSystem;
		}


        void MsquicManager::removeConnection(std::string accountId, std::string sessionId)
        {
            LOG_INFO("Remove MsquicSocket Request: Account=%s, SessionId=%s", accountId.c_str(), sessionId.c_str());

            auto it = msquicSocketMap.find(accountId);

            if (it == msquicSocketMap.end()) {
                LOG_WARNING("Connection already removed or not found: %s", accountId.c_str());
                return;
            }

            std::shared_ptr<MsquicSocketInterface> currentSocket = it->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARNING("Race Condition Detected! Ignore remove request. "
                    "Account: %s, RequestSessionId: %s, CurrentMapSessionId: %s",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return; 
            }

            msquicSocketMap.erase(it);

            int mapChannelIndex = hasher(accountId) % hashSize;

            int myChannelIndex = this->channelIndex;

            msquicSignalServer->postTaskAsync(mapChannelIndex, [accountId, myChannelIndex](std::shared_ptr<MsquicManager> manager) -> boost::asio::awaitable<void> {

                auto itIndex = manager->actorSocketMappingIndex.find(accountId);
                if (itIndex != manager->actorSocketMappingIndex.end()) {
                    if (itIndex->second == myChannelIndex) {
                        manager->actorSocketMappingIndex.erase(itIndex);
                        LOG_INFO("Global Index Removed: %s", accountId.c_str());
                    }
                    else {
                        LOG_WARNING("Global Index Mismatch (Race Condition), skip remove. Account: %s, Index points to: %d, Request from: %d",
                            accountId.c_str(), itIndex->second, myChannelIndex);
                    }
                }
                co_return;
                });
        }

	}

}