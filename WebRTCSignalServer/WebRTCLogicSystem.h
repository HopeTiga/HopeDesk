#pragma once

#include "WebRTCSignalSocket.h"
#include <unordered_map>
#include <memory>
#include <functional>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "concurrentqueue.h"


namespace hope {

	namespace core {

		enum class WebRTCRequestState {
			REGISTER = 0,
			REQUEST = 1,
			RESTART = 2,
			STOPREMOTE = 3,
			CLOSE = 4,
			CLOUD_GAME_SERVERS_REGISTER = 5,
			CLOUD_PROCESS_LOGIN = 6,
			CLOUD_PROCESS_LOGOUT = 7,
			CLOUD_PROCESS_HEARTBEAT = 8,
			CLOUD_GAME_START = 9,
			CLOUD_GAME_STOP = 10,
			USER_GET_GAMES_PROCESS_ID = 11
		};


		class WebRTCSignalData;

		class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
		{

		public:

			WebRTCLogicSystem();

			~WebRTCLogicSystem();

			WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

			void operator=(const WebRTCLogicSystem& logic) = delete;

			void postTaskAsync(std::shared_ptr<WebRTCSignalData> data);

			void RunEventLoop();

			boost::asio::io_context& getIoCompletePorts();

		private:

			void initHandlers();

			std::thread threads;

			boost::asio::io_context ioContext;

			std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;

			std::unordered_map<int, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>)>> webrtcHandlers;

		};
	}

}

