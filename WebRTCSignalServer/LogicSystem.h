#pragma once

#include "WebRTCSignalSocket.h"
#include <vector>
#include <thread>
#include <memory>
#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include "concurrentqueue.h"
#include "WebRTCSignalData.h"

namespace Hope {
	class LogicSystem : public std::enable_shared_from_this<LogicSystem>
	{

	public:

		~LogicSystem();

		LogicSystem(const LogicSystem& logic) = delete;

		void operator=(const LogicSystem& logic) = delete;

		void postMessageToQueue(std::shared_ptr<WebRTCSignalData> data, int channelIndex);

		void initializeThreads();

		static std::shared_ptr<LogicSystem> getInstance() {
			static std::shared_ptr<LogicSystem> instance = std::shared_ptr<LogicSystem>(new LogicSystem());
			return instance;
		}

	private:

		LogicSystem(size_t minSize = std::thread::hardware_concurrency()*2);

		std::vector<moodycamel::ConcurrentQueue<std::shared_ptr<WebRTCSignalData>>> taskChannels;

		std::vector<std::thread> threads;

		std::atomic<bool> isStop;

		size_t size;

		std::vector<boost::asio::io_context> ioContexts;

		std::vector<std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>> works;

		std::vector<std::atomic<bool>> readyVector;

		std::vector<std::unique_ptr<boost::asio::experimental::concurrent_channel<void(boost::system::error_code)>>> channels;

	};

}

