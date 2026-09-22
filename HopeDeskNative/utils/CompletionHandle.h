#pragma once

#include <exception>
#include <source_location>

#include "Utils.h"

namespace hope {

	struct CompletionHandle {

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
