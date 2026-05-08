#pragma once
#include <unordered_set>
#include <memory>
#include <atomic>
#include <thread>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <signal.h>
#endif

#include <boost/asio.hpp>

#include "Utils.h"

namespace hope {

	namespace protect {

		class ProtectProcess
		{
		public:

			ProtectProcess(boost::asio::io_context& ioContext);

			~ProtectProcess();

			void createProcess(const std::string& exePath);

			void killChildProcess();

		private:

			std::thread threads;

			std::thread monitorThread;

			boost::asio::io_context& ioContext;

			std::atomic<bool> eventLoop{ false };

#ifdef _WIN32
			PROCESS_INFORMATION childProcessInfo{ 0 };
#else
			pid_t childPid{ -1 };
#endif

			std::atomic<bool> hasChildProcess{ false };

			std::string currentExePath;
		};

	}

}