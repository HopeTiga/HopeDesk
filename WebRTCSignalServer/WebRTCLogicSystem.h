#pragma once

#include <unordered_map>
#include <memory>
#include <functional>
#include <utility>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include "concurrentqueue.h"

#include "WebRTCHashMap.h"


namespace hope {

	namespace mysql {

		class MsquicMysqlManager;

	}

	namespace core {

		class WebRTCSignalData;

		class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
		{

		public:

			WebRTCLogicSystem(boost::asio::io_context& ioContext);

			~WebRTCLogicSystem();

			WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

			void operator=(const WebRTCLogicSystem& logic) = delete;

			void postTaskAsync(std::shared_ptr<hope::core::WebRTCSignalData> data);

			void RunEventLoop();

			boost::asio::io_context& getIoCompletePorts();

		private:

			void initHandlers();

			boost::asio::io_context& ioContext;

			hope::utils::WebRTCHashMap<int, std::function<boost::asio::awaitable<void>(std::shared_ptr<hope::core::WebRTCSignalData>)>> webrtcHandlers;

		};
	}

}

