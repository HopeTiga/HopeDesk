#pragma once
#include <memory>
#include <atomic>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <boost/mysql/connection_pool.hpp>

#include "MysqlConfig.h"

namespace hope {

	namespace storage {

		class MysqlManagerPools : public std::enable_shared_from_this<MysqlManagerPools>
		{
		public:

			struct ScopedMysqlConnection {
				boost::mysql::pooled_connection connection;

				ScopedMysqlConnection(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection& operator=(const ScopedMysqlConnection&) = delete;
				ScopedMysqlConnection(ScopedMysqlConnection&&) noexcept = default;
				ScopedMysqlConnection& operator=(ScopedMysqlConnection&&) noexcept = default;

				explicit ScopedMysqlConnection(boost::mysql::pooled_connection connection)
					: connection(std::move(connection)) {
				}

				boost::mysql::any_connection* getConnection() noexcept {
					return connection.valid() ? &connection.get() : nullptr;
				}
			};

		public:

			MysqlManagerPools(boost::asio::io_context& ioContext, MysqlConfig mysqlConfig);

			~MysqlManagerPools();

			boost::asio::awaitable<ScopedMysqlConnection> getTransactionMysqlManager();

		private:

			boost::asio::io_context& ioContext;

			MysqlConfig mysqlConfig;

			std::shared_ptr<boost::mysql::connection_pool> pool;
		};

	}

}