#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "concurrentqueue.h"

#include "MsquicMysqlManager.h"

namespace hope {

	namespace mysql {
	
		class MsquicMysqlManagerPools : public std::enable_shared_from_this<MsquicMysqlManagerPools>
		{
			struct ScopedMysqlConnection {
				std::shared_ptr<MsquicMysqlManager> conn;
				MsquicMysqlManagerPools* pool;

				// 禁用拷贝，只允许移动（确保所有权唯一）
				ScopedMysqlConnection(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection& operator=(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection(ScopedMysqlConnection&&) = default;

				// 构造函数
				ScopedMysqlConnection(std::shared_ptr<MsquicMysqlManager> c, MsquicMysqlManagerPools* p)
					: conn(std::move(c)), pool(p) {
				}

				// 析构函数：核心魔法，离开作用域自动归还
				~ScopedMysqlConnection() {
					if (conn && pool) {
						pool->returnTransactionMysqlManager(std::move(conn));
					}
				}

				// 重载 -> 运算符，让你像使用指针一样使用它
				MsquicMysqlManager* operator->() { return conn.get(); }
			};

		public:

			static std::shared_ptr<MsquicMysqlManagerPools> getInstance() {
			
				static std::shared_ptr<MsquicMysqlManagerPools> instance = std::make_shared<MsquicMysqlManagerPools>();

				return instance;
			}

			boost::asio::awaitable<MsquicMysqlManagerPools::ScopedMysqlConnection> getTransactionMysqlManager();

			void returnTransactionMysqlManager(std::shared_ptr<MsquicMysqlManager> mysqlManager);

			MsquicMysqlManagerPools(size_t size = std::thread::hardware_concurrency());

			~MsquicMysqlManagerPools();

		private:

			boost::asio::io_context ioContext;

			std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard;

			std::thread ioThread;

			std::atomic<size_t> size;

			moodycamel::ConcurrentQueue<std::shared_ptr<MsquicMysqlManager>> transactionMysqlManagers { 1 };

			std::shared_ptr<boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::shared_ptr<MsquicMysqlManager>)>> transactionChannels;


		};

	}

}

