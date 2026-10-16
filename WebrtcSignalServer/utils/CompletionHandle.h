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
		void operator()(std::exception_ptr exceptionPtr, Args&&... /*value*/) const noexcept {

			if (!exceptionPtr) {

				return;

			}

			try {

				std::rethrow_exception(exceptionPtr);

			}
			catch (const std::exception& exception) {

				LOG_ERROR_FROM(spawnLocation, "CoSpawn Exception: {}", exception.what());

			}
			catch (...) {

				LOG_ERROR_FROM(spawnLocation, "CoSpawn Exception: Unknown Exception");

			}

		}

	private:

		std::source_location spawnLocation;

	};

}
