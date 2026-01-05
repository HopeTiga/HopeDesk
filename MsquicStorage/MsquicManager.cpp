#include "MsquicManager.h"

#include <boost/asio.hpp>

#include "MsquicServer.h"
#include "MsquicSocket.h"

#include "Utils.h"

namespace hope {

	namespace quic {
	
		MsquicManager::MsquicManager(size_t channelIndex, boost::asio::io_context& ioContext,MsquicServer * msquicServer) 
			: channelIndex(channelIndex)
			, ioContext(ioContext)
			, msquicServer(msquicServer)
			, localRouteCache([](std::string) -> int {
			return -1;
				}, 100)
		{
			logicSystem = std::make_shared<hope::handle::MsquicLogicSystem>(ioContext);

			logicSystem->RunEventLoop();
		}

		MsquicManager::~MsquicManager()
		{
			actorSocketMappingIndex.clear();

			msquicSocketInterfaceMap.clear();

		}

		std::shared_ptr<hope::handle::MsquicLogicSystem> MsquicManager::getMsquicLogicSystem()
		{
			return logicSystem;
		}

		void MsquicManager::removeConnection(std::string accountId)
		{

            LOG_INFO("Remove MsquicSocket: %s", accountId.c_str());

			auto it = msquicSocketInterfaceMap.find(accountId);

			if (it == msquicSocketInterfaceMap.end()) {

				LOG_WARNING("Connection already removed: %s", accountId.c_str());

				return;
			}

			if (it->second) {

				closingSockets.insert(it->second);

			}

			

			msquicSocketInterfaceMap.erase(it);

            int mapChannelIndex = hasher(accountId) % hashSize;

            LOG_INFO("Start Async Post Task: %d", mapChannelIndex);

            msquicServer->postTaskAsync(mapChannelIndex, [self = shared_from_this(), accountId](std::shared_ptr<MsquicManager> manager) -> boost::asio::awaitable<void> {

                manager->actorSocketMappingIndex.erase(accountId);

                co_return;

                });

		}

		void MsquicManager::finalizeConnection(std::shared_ptr<MsquicSocketInterface> socket)
		{
			closingSockets.erase(socket);

			LOG_INFO("MsquicSocket Finalized And Destroyed.");
		}

	}

}