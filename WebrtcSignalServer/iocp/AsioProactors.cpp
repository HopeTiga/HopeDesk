#include "AsioProactors.h"
#include <iostream>
#include "../utils/Utils.h"

namespace hope {
	namespace iocp {

		size_t AsioProactors::sIoSize = std::thread::hardware_concurrency();

		size_t AsioProactors::sLogicSize = std::thread::hardware_concurrency();

		void AsioProactors::init(size_t size) {
			sIoSize = size;
			sLogicSize = size;
		}

		AsioProactors::AsioProactors(size_t size)
			: size(size)
			, ioContexts(size)
			, works(size)
			, threads(size)
			, ioPressures(size) {

			for (int i = 0; i < size; i++) {

				ioContexts[i] = std::make_unique<boost::asio::io_context>(1);

				std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
					boost::asio::make_work_guard(*ioContexts[i].get())
				);

				works[i] = std::move(work);

				threads[i] = std::thread([this, i]() {
					ioContexts[i]->run();
					});
			}

		}

		AsioProactors::~AsioProactors() {
			stop();
		}

		// 温和关闭:仅释放 work guard,让各 io_context 线程跑完已提交的 handler 后
		// run() 自然返回。不调 io_context::stop(),不丢弃 pending 操作。
		void AsioProactors::releaseWork() {
			for (auto& work : works) {

				if (work) {
					work.reset();
				}
			}
		}

		// 核爆兜底:立即 stop,丢弃未执行 handler,并 join 线程
		void AsioProactors::stop() {

			releaseWork();

			for (auto& context : ioContexts) {
				context->stop();
			}

			for (auto& t : threads) {
				if (t.joinable()) {
					t.join();
				}
			}
		}

		std::pair<int, boost::asio::io_context&> AsioProactors::getIoCompletePorts() {
			size_t current = loadBalancing.fetch_add(1);
			size_t index = current % size;
			ioPressures[index]++;
			return { static_cast<int>(index), *ioContexts[index] };
		}

		boost::asio::io_context& AsioProactors::getIoCompletePort(size_t channelIndex)
		{
			return *ioContexts[channelIndex];
		}

	}
}