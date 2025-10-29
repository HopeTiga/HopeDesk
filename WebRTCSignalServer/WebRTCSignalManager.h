#pragma once
#define TBB_PREVIEW_CONCURRENT_LRU_CACHE 1

#include <vector>
#include <memory>

#include <boost/asio.hpp>

#include <tbb/concurrent_unordered_map.h>
#include <tbb/concurrent_lru_cache.h>

#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"
#include "WebRTCLogicSystem.h"
#include "WebRTCMysqlManager.h"


namespace hope {
	namespace core {

		class WebRTCSignalServer;

		class WebRTCSignalManager : public std::enable_shared_from_this<WebRTCSignalManager>
		{
			friend class WebRTCLogicSystem;

		public:

			WebRTCSignalManager(boost::asio::io_context& ioContext, int channelIndex, WebRTCSignalServer* webrtcSignalServer);

			~WebRTCSignalManager();

			std::shared_ptr<WebRTCSignalSocket> generateWebRTCSignalSocket();

			boost::asio::io_context& getIoCompletePorts();

			void removeConnection(const std::string& accountID);

			tbb::concurrent_unordered_map<std::string, int>& getActorSocketMappingIndex();

			std::shared_ptr<WebRTCLogicSystem> getWebRTCLogicSystem();

		private:

			boost::asio::io_context& ioContext;

			int channelIndex;

			tbb::concurrent_unordered_map<std::string, std::shared_ptr<WebRTCSignalSocket>> webrtcSignalSocketMap;

			WebRTCSignalServer* webrtcSignalServer;

			size_t hashSize = std::thread::hardware_concurrency() * 2;

			tbb::concurrent_unordered_map<std::string, int> actorSocketMappingIndex;

			tbb::concurrent_lru_cache<std::string, int> localRouteCache;

			std::hash<std::string> hasher;

			std::shared_ptr<WebRTCLogicSystem> webrtcLogicSystem;

			std::unique_ptr<WebRTCMysqlManager> webrtcMysqlManager;

		};
	}
}

