#pragma once
#include<boost/asio.hpp>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "SchedulerConfig.h"

namespace hope {
	namespace executor {
		class SchedulerContext {

		public:

			static void init(const SchedulerConfig& schedulerConfig);

			static SchedulerContext* getInstance() {
				static SchedulerContext instance(sIoSize, sIoCpuIndexes);
				return &instance;
			}

#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC

			static SchedulerContext* getLogicInstance() {
				static SchedulerContext instance(sLogicSize, sLogicCpuIndexes);
				return &instance;
			}

#endif

			~SchedulerContext();

			void stop();

			void releaseWork();

			SchedulerContext(const SchedulerContext& schedulerContext) = delete;

			SchedulerContext& operator=(const SchedulerContext& schedulerContext) = delete;

			std::pair<int, boost::asio::io_context&> getIoCompletePorts();

			boost::asio::io_context& getIoCompletePort(size_t channelIndex);

		private:

			SchedulerContext(size_t size, const std::vector<size_t>& cpuIndexes);

			static size_t sIoSize;

			static size_t sLogicSize;

			static std::vector<size_t> sIoCpuIndexes;

			static std::vector<size_t> sLogicCpuIndexes;

			std::vector<std::unique_ptr<boost::asio::io_context>> ioContexts;

			std::vector<std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>> works;

			std::vector<std::thread> threads;

			std::vector<std::atomic<size_t>> ioPressures;

			std::mutex mutexs;

			size_t size;

			std::atomic<size_t> loadBalancing = 0;
		};
	}
}
