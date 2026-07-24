#include "WebRTCMysqlManagerPools.h"
#include "MysqlConfig.h"
#include "../utils/Utils.h"


namespace hope {

	namespace mysql {

		WebRTCMysqlManagerPools::WebRTCMysqlManagerPools(boost::asio::io_context& ioContext)
			: ioContext(ioContext) {

			const MysqlConfig& mysqlConfig = globalMysqlConfig;

			boost::mysql::pool_params params;
			params.server_address.emplace_host_and_port(mysqlConfig.host, mysqlConfig.port);
			params.username = mysqlConfig.username;
			params.password = mysqlConfig.password;
			params.database = mysqlConfig.database;

			params.ssl = boost::mysql::ssl_mode::disable;
			params.multi_queries = mysqlConfig.multiQueries;

			params.initial_size = mysqlConfig.poolInitialSize;
			params.max_size = mysqlConfig.poolMaxSize;

			params.connect_timeout = std::chrono::seconds(mysqlConfig.connectTimeoutSeconds);
			params.ping_interval = std::chrono::seconds(mysqlConfig.pingIntervalSeconds);
			params.ping_timeout = std::chrono::seconds(mysqlConfig.pingTimeoutSeconds);

			params.thread_safe = false;

			pool = std::make_shared<boost::mysql::connection_pool>(ioContext, std::move(params));

			boost::asio::co_spawn(ioContext,
				[pool = this->pool]() -> boost::asio::awaitable<void> {
					try {
						co_await pool->async_run(boost::asio::use_awaitable);
					}
					catch (const std::exception& e) {
						LOG_ERROR("MySQL connection_pool async_run exited: %s", e.what());
					}
				}, boost::asio::detached);

			LOG_DEBUG("MySQL connection_pool created (initial=%zu, max=%zu) on io_context %p",
				params.initial_size, params.max_size, static_cast<void*>(&ioContext));
		}

		WebRTCMysqlManagerPools::~WebRTCMysqlManagerPools() {

			if (pool) {
				boost::asio::post(ioContext, [pool = this->pool]() {
					pool->cancel();
					});
			}

			LOG_INFO("MySQL connection_pool cancel posted");
		}

		boost::asio::awaitable<WebRTCMysqlManagerPools::ScopedMysqlConnection> WebRTCMysqlManagerPools::getTransactionMysqlManager()
		{
			boost::mysql::pooled_connection pooledConn;

			try {
				pooledConn = co_await pool->async_get_connection(boost::asio::use_awaitable);
			}
			catch (const std::exception& e) {
				LOG_ERROR("WebRTCMysqlManagerPools::getTransactionMysqlManager failed: %s", e.what());
			}

			co_return ScopedMysqlConnection(std::move(pooledConn));
		}

	}

}