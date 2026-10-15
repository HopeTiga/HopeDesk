#pragma once
#include<boost/asio.hpp>
#include <memory>
#include <thread>
#include <vector>

#include "SchedulerConfig.h"

namespace hope {
	namespace executor {

		class SchedulerContexts {

		public:

			static void init(const SchedulerConfig& schedulerConfig);

			static SchedulerContexts* getInstance() {
				static SchedulerContexts instance(sIoSize, sIoCpuIndexes);
				return &instance;
			}

#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC

			static SchedulerContexts* getLogicInstance() {
				static SchedulerContexts instance(sLogicSize, sLogicCpuIndexes);
				return &instance;
			}

#endif

			~SchedulerContexts();

			void stop();

			void releaseWork();

			SchedulerContexts(const SchedulerContexts& schedulerContexts) = delete;

			SchedulerContexts& operator=(const SchedulerContexts& schedulerContexts) = delete;

			std::pair<int, boost::asio::io_context&> getSchedulerContext();

			boost::asio::io_context& getSchedulerContext(size_t channelIndex);

		private:

			SchedulerContexts(size_t size, const std::vector<size_t>& cpuIndexes);

			static size_t sIoSize;

			static size_t sLogicSize;

			static std::vector<size_t> sIoCpuIndexes;

			static std::vector<size_t> sLogicCpuIndexes;

			std::vector<std::unique_ptr<boost::asio::io_context>> ioContexts;

			std::vector<std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>> executorWorkGuards;

			std::vector<std::thread> threads;

			std::vector<std::atomic<size_t>> ioPressures;

			size_t size;

			std::atomic<size_t> loadBalancing = 0;
		};
	}
}
