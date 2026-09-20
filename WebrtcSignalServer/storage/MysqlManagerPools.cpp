
#include "MysqlManagerPools.h"
#include "MysqlConfig.h"
#include "../utils/Utils.h"


namespace hope {

	namespace storage {

		MysqlManagerPools::MysqlManagerPools(boost::asio::io_context& ioContext, MysqlConfig mysqlConfig)
			: ioContext(ioContext)
			, mysqlConfig(std::move(mysqlConfig)) {

			boost::mysql::pool_params params;
			params.server_address.emplace_host_and_port(this->mysqlConfig.host, this->mysqlConfig.port);
			params.username = this->mysqlConfig.username;
			params.password = this->mysqlConfig.password;
			params.database = this->mysqlConfig.database;

			params.ssl = boost::mysql::ssl_mode::disable;
			params.multi_queries = this->mysqlConfig.multiQueries;

			params.initial_size = this->mysqlConfig.poolInitialSize;
			params.max_size = this->mysqlConfig.poolMaxSize;

			params.connect_timeout = std::chrono::seconds(this->mysqlConfig.connectTimeoutSeconds);
			params.ping_interval = std::chrono::seconds(this->mysqlConfig.pingIntervalSeconds);
			params.ping_timeout = std::chrono::seconds(this->mysqlConfig.pingTimeoutSeconds);

			params.thread_safe = false;

			pool = std::make_shared<boost::mysql::connection_pool>(ioContext, std::move(params));

			boost::asio::co_spawn(ioContext,
				[pool = this->pool]() -> boost::asio::awaitable<void> {
					try {
						co_await pool->async_run(boost::asio::use_awaitable);
					}
					catch (const std::exception& e) {
						LOG_ERROR("MySQL ConnectionPool AsyncRun Exited: {}", e.what());
					}
				}, boost::asio::detached);

			LOG_DEBUG("MySQL ConnectionPool Created (Initial={}, Max={}) On IoContext {}",
				params.initial_size, params.max_size, static_cast<void*>(&ioContext));
		}

		MysqlManagerPools::~MysqlManagerPools() {

			if (pool) {
				boost::asio::post(ioContext, [pool = this->pool]() {
					pool->cancel();
					});
			}

			LOG_INFO("MySQL ConnectionPool Cancel Posted");
		}

		boost::asio::awaitable<MysqlManagerPools::ScopedMysqlConnection> MysqlManagerPools::getTransactionMysqlManager()
		{
			boost::mysql::pooled_connection pooledConn;

			try {
				pooledConn = co_await pool->async_get_connection(boost::asio::use_awaitable);
			}
			catch (const std::exception& e) {
				LOG_ERROR("Failed: {}", e.what());
			}

			co_return ScopedMysqlConnection(std::move(pooledConn));
		}

	}

}