#pragma once

#include <unordered_map>
#include <memory>
#include <functional>
#include <utility>

#include <boost/asio.hpp>

#include <absl/container/flat_hash_map.h>

#include "WebRTCMysqlManagerPools.h"


namespace hope {

	namespace core {

		class WebRTCSignalData;

		class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
		{

		public:

			WebRTCLogicSystem(boost::asio::io_context& ioContext,int channelIndex);

			~WebRTCLogicSystem();

			WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

			void operator=(const WebRTCLogicSystem& logic) = delete;

			void postTaskAsync(std::shared_ptr<hope::core::WebRTCSignalData> data);

			boost::asio::io_context& getIoCompletePorts();

			void initHandlers();

		private:

			boost::asio::io_context& ioContext;

			int channelIndex;

			absl::flat_hash_map<int, std::function<boost::asio::awaitable<void>(std::shared_ptr<hope::core::WebRTCSignalData>)>> webrtcHandlers;

			std::shared_ptr<hope::mysql::WebRTCMysqlManagerPools> webrtcMysqlManagerPools;
		};
	}

}

