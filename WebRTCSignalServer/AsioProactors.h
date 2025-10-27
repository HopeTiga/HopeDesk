#pragma once
#include<boost/asio.hpp>
#include <memory>
#include <mutex>
#include <thread>

namespace Hope {
	class AsioProactors {

	public:
		static AsioProactors* getInstance() {
			static AsioProactors instance;
			return &instance;
		}

		~AsioProactors();

		void stop();

		AsioProactors(const AsioProactors& asioProactors) = delete;

		AsioProactors& operator=(const AsioProactors& asioProactors) = delete;

		std::pair<int, boost::asio::io_context&> getIoCompletePorts();

	private:

		AsioProactors(size_t size = std::thread::hardware_concurrency() * 2);

		std::vector<boost::asio::io_context> ioContexts;

		// 使用新的 work guard 替代已废弃的 io_context::work
		std::vector<std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>> works;

		std::vector<std::thread> threads;
		std::vector<std::atomic<size_t>> ioPressures;
		std::mutex mutexs;
		size_t size;
		std::atomic<size_t> loadBalancing = 0;
		std::atomic<bool> isStop;
	};
}