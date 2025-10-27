#pragma once

#include "WebRTCSignalSocket.h"
#include <vector>
#include <thread>
#include <memory>
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "concurrentqueue.h"


namespace Hope {

	class WebRTCSignalData;

	class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
	{

	public:

		WebRTCLogicSystem();

		~WebRTCLogicSystem();

		WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

		void operator=(const WebRTCLogicSystem& logic) = delete;

		void postMessageToQueue(std::shared_ptr<WebRTCSignalData> data);

		void RunEventLoop();

		boost::asio::io_context& getIoCompletePorts();

	private:

	

		std::thread threads;

		boost::asio::io_context ioContext;

		std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;

	};

}

