#pragma once
#include<boost/asio.hpp>
#include <memory>
#include <mutex>
#include <thread>

namespace hope {
	namespace iocp {
		class AsioProactors {

		public:

			static void init(size_t size);

			static AsioProactors* getInstance() {
				static AsioProactors instance(sIoSize);
				return &instance;
			}


#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC

			static AsioProactors* getLogicInstance() {
				static AsioProactors instance(sLogicSize);
				return &instance;
			}

#endif

	
			~AsioProactors();

			void stop();

			AsioProactors(const AsioProactors& asioProactors) = delete;

			AsioProactors& operator=(const AsioProactors& asioProactors) = delete;

			std::pair<int, boost::asio::io_context&> getIoCompletePorts();

			boost::asio::io_context& getIoCompletePort(size_t channelIndex);

		private:

			AsioProactors(size_t size);

			static size_t sIoSize;
			
			static size_t sLogicSize;

			std::vector<std::unique_ptr<boost::asio::io_context>> ioContexts;

			std::vector<std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>> works;

			std::vector<std::thread> threads;

			std::vector<std::atomic<size_t>> ioPressures;

			std::mutex mutexs;

			size_t size;

			std::atomic<size_t> loadBalancing = 0;

			std::atomic<bool> isStop;
		};
	}
}