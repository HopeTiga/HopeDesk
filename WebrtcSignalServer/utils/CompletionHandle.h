#pragma once

#include <exception>
#include <source_location>

#include "Utils.h"

namespace hope {

	struct CompletionHandle {

		// 位置必须记在构造点：默认参数里的 current() 在调用点求值，报出来才是 co_spawn 那一行。
		// 直接用 LOG_ERROR 的话，所有协程的异常都会显示成这个文件里的 catch 行。
		explicit CompletionHandle(std::source_location location = std::source_location::current())
			: spawnLocation(location)
		{

		}

		template <typename... Args>
		void operator()(std::exception_ptr exception, Args&&... /*value*/) const noexcept {

			if (!exception) {

				return;

			}

			try {

				std::rethrow_exception(exception);

			}
			catch (const std::exception& e) {

				LOG_ERROR_FROM(spawnLocation, "CoSpawn Exception: {}", e.what());

			}
			catch (...) {

				LOG_ERROR_FROM(spawnLocation, "CoSpawn Exception: unknown exception");

			}

		}

	private:

		std::source_location spawnLocation;

	};

}
