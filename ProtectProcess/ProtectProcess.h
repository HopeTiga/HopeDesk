#pragma once
#include <unordered_set>
#include <memory>
#include <atomic>
#include <thread>

#include <boost/asio.hpp>

#include "Utils.h"

namespace hope {
	
	namespace protect{

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

			PROCESS_INFORMATION childProcessInfo{ 0 };

			std::atomic<bool> hasChildProcess{ false };

			std::string currentExePath;
		};

	}

}