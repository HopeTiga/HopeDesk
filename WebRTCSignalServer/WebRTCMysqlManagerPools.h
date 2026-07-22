#pragma once

#include <memory>
#include <atomic>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <boost/mysql/connection_pool.hpp>

namespace hope {

	namespace mysql {

		class WebRTCMysqlManagerPools : public std::enable_shared_from_this<WebRTCMysqlManagerPools>
		{
		public:

			struct ScopedMysqlConnection {
				boost::mysql::pooled_connection conn;

				ScopedMysqlConnection(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection& operator=(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection(ScopedMysqlConnection&&) noexcept = default;
				ScopedMysqlConnection& operator=(ScopedMysqlConnection&&) noexcept = default;

				explicit ScopedMysqlConnection(boost::mysql::pooled_connection c)
					: conn(std::move(c)) {
				}

				boost::mysql::any_connection* getConnection() noexcept {
					return conn.valid() ? &conn.get() : nullptr;
				}
			};

		public:

			WebRTCMysqlManagerPools(boost::asio::io_context& ioContext);

			~WebRTCMysqlManagerPools();

			boost::asio::awaitable<ScopedMysqlConnection> getTransactionMysqlManager();

		private:

			boost::asio::io_context& ioContext;

			std::shared_ptr<boost::mysql::connection_pool> pool;
		};

	}

}