#pragma once
#define TBB_PREVIEW_CONCURRENT_LRU_CACHE 1
#include <memory>
#include <string>

#include <tbb/concurrent_lru_cache.h>

#include "MsquicLogicSystem.h"
#include "MsquicSocketInterface.h"

#include "MsquicHashMap.h"
#include "MsquicHashSet.h"

namespace hope {

	namespace quic {

		class MsquicSignalServer;

		class MsquicData;

		class MsquicManager : public std::enable_shared_from_this<MsquicManager>
		{
			friend class hope::handle::MsquicLogicSystem;
		public:

			MsquicManager(size_t channelIndex, boost::asio::io_context& ioContext, MsquicSignalServer* msquicSignalServer);

			~MsquicManager();

			std::shared_ptr<hope::handle::MsquicLogicSystem> getMsquicLogicSystem();

			void removeConnection(std::string accountId,std::string sessionId);

		private:

			boost::asio::io_context& ioContext;

			MsquicSignalServer* msquicSignalServer;

			size_t channelIndex;

			std::shared_ptr<hope::handle::MsquicLogicSystem> logicSystem;

			hope::utils::MsquicHashMap<std::string, std::shared_ptr<MsquicSocketInterface>> msquicSocketMap;

			size_t hashSize = std::thread::hardware_concurrency();

			hope::utils::MsquicHashMap<std::string, int> actorSocketMappingIndex;

			tbb::concurrent_lru_cache<std::string, int> localRouteCache;

			std::hash<std::string> hasher;

		};

	}

} // namespace hope
