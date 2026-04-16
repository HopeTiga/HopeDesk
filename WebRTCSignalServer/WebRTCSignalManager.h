#pragma once
#define TBB_PREVIEW_CONCURRENT_LRU_CACHE 1
#include <memory>
#include <string>

#include <tbb/concurrent_lru_cache.h>

#include "WebRTCLogicSystem.h"
#include "WebRTCSignalSocket.h"

#include "WebRTCHashMap.h"
#include "WebRTCHashSet.h"

namespace hope {

	namespace core {

		class WebRTCSignalServer;

		class WebRTCSignalData;

		class WebRTCSignalManager : public std::enable_shared_from_this<WebRTCSignalManager>
		{
			friend class hope::core::WebRTCLogicSystem;
		public:

			WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer* webrtcSignalServer);

			~WebRTCSignalManager();

			std::shared_ptr<hope::core::WebRTCLogicSystem> getLogicSystem();

			void removeConnection(std::string accountId, std::string sessionId, bool needClear = true);

			int getChannelIndex();

		private:

			boost::asio::io_context& ioContext;

			WebRTCSignalServer* webrtcSignalServer;

			size_t channelIndex;

			std::shared_ptr<hope::core::WebRTCLogicSystem> logicSystem;

			hope::utils::WebRTCHashMap<std::string, std::shared_ptr<WebRTCSignalSocket>> webrtcSocketMap;

			size_t hashSize = std::thread::hardware_concurrency();

			hope::utils::WebRTCHashMap<std::string, int> actorSocketMappingIndex;

			std::hash<std::string> hasher;

		};

	}

} // namespace hope
