#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "concurrentqueue.h"

#include "WebRTCMysqlManager.h"

namespace hope {

	namespace mysql {

		class WebRTCMysqlManagerPools : public std::enable_shared_from_this<WebRTCMysqlManagerPools>
		{
			struct ScopedMysqlConnection {
				std::shared_ptr<WebRTCMysqlManager> conn;
				WebRTCMysqlManagerPools* pool;

				// 禁用拷贝，只允许移动（确保所有权唯一）
				ScopedMysqlConnection(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection& operator=(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection(ScopedMysqlConnection&&) = default;

				// 构造函数
				ScopedMysqlConnection(std::shared_ptr<WebRTCMysqlManager> c, WebRTCMysqlManagerPools* p)
					: conn(std::move(c)), pool(p) {
				}

				// 析构函数：核心魔法，离开作用域自动归还
				~ScopedMysqlConnection() {
					if (conn && pool) {
						pool->returnTransactionMysqlManager(std::move(conn));
					}
				}

				WebRTCMysqlManager* operator->() { return conn.get(); }
			};

		public:

			boost::asio::awaitable<WebRTCMysqlManagerPools::ScopedMysqlConnection> getTransactionMysqlManager();

			void returnTransactionMysqlManager(std::shared_ptr<WebRTCMysqlManager> mysqlManager);

			WebRTCMysqlManagerPools(boost::asio::io_context& ioContext, size_t size = 2);

			~WebRTCMysqlManagerPools();

		private:

			boost::asio::io_context& ioContext;

			std::atomic<size_t> size;

			moodycamel::ConcurrentQueue<std::shared_ptr<WebRTCMysqlManager>> transactionMysqlManagers{ 1 };

			std::shared_ptr<boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::shared_ptr<WebRTCMysqlManager>)>> transactionChannels;

		};

	}

}

