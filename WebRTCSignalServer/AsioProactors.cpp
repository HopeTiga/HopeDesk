#include "AsioProactors.h"
#include <iostream>
#include "Utils.h"

AsioProactors::AsioProactors(size_t size) :size(size),
 ioContexts(size), works(size), threads(size), ioPressures(size), isStop(false) {

	for (int i = 0; i < size; i++) {
		// 使用新的 work guard API
		auto work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
			boost::asio::make_work_guard(ioContexts[i])
		);

		works[i] = std::move(work);
		threads[i] = std::thread([this, i]() {
			ioContexts[i].run();
			});
	}

}

AsioProactors::~AsioProactors() {
	stop();
}

void AsioProactors::stop() {
	isStop = true;

	for (auto& work : works) {
		// 重置 work guard，这会让 io_context 停止运行
		if (work) {
			work.reset();
		}
	}

	// 明确停止所有 io_context
	for (auto& context : ioContexts) {
		context.stop();
	}

	for (auto& t : threads) {
		if (t.joinable()) {
			t.join();
		}
	}
}

std::pair<int, boost::asio::io_context&> AsioProactors::getIoComplatePorts() {
	int balancing = loadBalancing++;
	boost::asio::io_context& context = ioContexts[balancing % size];
	ioPressures[balancing % size]++;
	if (loadBalancing >= size) {
		loadBalancing = 0;
	}
	return std::pair<int,boost::asio::io_context&>(balancing, context);
}