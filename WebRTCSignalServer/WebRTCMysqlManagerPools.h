#pragma once

#include <memory>
#include <vector>
#include "concurrentqueue.h"

#include "WebRTCMysqlManager.h"

namespace hope {

	namespace core {
	
		class WebRTCMysqlManagerPools : public std::enable_shared_from_this<WebRTCMysqlManagerPools>
		{

		public:

			static std::shared_ptr<WebRTCMysqlManagerPools> getInstance() {
			
				static std::shared_ptr<WebRTCMysqlManagerPools> instance = std::make_shared<WebRTCMysqlManagerPools>();

				return instance;
			}

			std::shared_ptr<WebRTCMysqlManager> getMysqlManager();

			std::shared_ptr<WebRTCMysqlManager> getTransactionMysqlManager();

			void returnTransactionMysqlManager(std::shared_ptr<WebRTCMysqlManager> mysqlManager);

			WebRTCMysqlManagerPools(size_t size = std::thread::hardware_concurrency());

			~WebRTCMysqlManagerPools();

		private:

			std::atomic<size_t> size;

			moodycamel::ConcurrentQueue<std::shared_ptr<WebRTCMysqlManager>> transactionMysqlManagers { 1 };

			std::vector<std::shared_ptr<WebRTCMysqlManager>> mysqlManagers;

			std::atomic<size_t> loadBalancing{ 0 };

		};

	}

}

