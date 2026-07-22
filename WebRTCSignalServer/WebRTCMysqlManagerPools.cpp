#include "WebRTCMysqlManagerPools.h"
#include "ConfigManager.h"
#include "Utils.h"


namespace hope {

	namespace mysql {

		WebRTCMysqlManagerPools::WebRTCMysqlManagerPools(boost::asio::io_context& ioContext)
			: ioContext(ioContext) {

			boost::mysql::pool_params params;
			params.server_address.emplace_host_and_port(
				ConfigManager::Instance().GetString("Mysql.host"),
				static_cast<unsigned short>(ConfigManager::Instance().GetInt("Mysql.port")));
			params.username = ConfigManager::Instance().GetString("Mysql.username");
			params.password = ConfigManager::Instance().GetString("Mysql.password");
			params.database = ConfigManager::Instance().GetString("Mysql.database");

			params.ssl = boost::mysql::ssl_mode::disable;
			params.multi_queries = ConfigManager::Instance().GetInt("Mysql.multiQueries", 0) != 0;

			params.initial_size = static_cast<std::size_t>(ConfigManager::Instance().GetInt("Mysql.poolInitialSize", 2));
			params.max_size = static_cast<std::size_t>(ConfigManager::Instance().GetInt("Mysql.poolMaxSize", 16));

			params.connect_timeout = std::chrono::seconds(
				ConfigManager::Instance().GetInt("Mysql.connectTimeoutSeconds", 20));
			params.ping_interval = std::chrono::seconds(
				ConfigManager::Instance().GetInt("Mysql.pingIntervalSeconds", 3600));
			params.ping_timeout = std::chrono::seconds(
				ConfigManager::Instance().GetInt("Mysql.pingTimeoutSeconds", 10));

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