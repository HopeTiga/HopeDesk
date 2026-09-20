#pragma once

#include <string>
#include <cstddef>

#include "../utils/ConfigManager.h"

namespace hope {

	namespace storage {

		struct MysqlConfig {

			std::string host{"127.0.0.1"};

			unsigned short port{3306};

			std::string username;

			std::string password;

			std::string database;

			bool multiQueries{false};

			std::size_t poolInitialSize{2};

			std::size_t poolMaxSize{16};

			int connectTimeoutSeconds{20};

			int pingIntervalSeconds{3600};

			int pingTimeoutSeconds{10};

		};

		inline void loadMysqlConfig(MysqlConfig& mysqlConfig, const hope::utils::ConfigManager& configManager) {

			mysqlConfig.host = configManager.GetString("Mysql.host", mysqlConfig.host);

			// 越界会截断成合法但错误的端口
			const int port = configManager.GetInt("Mysql.port", static_cast<int>(mysqlConfig.port));
			mysqlConfig.port = (port > 0 && port <= 65535) ? static_cast<unsigned short>(port) : mysqlConfig.port;

			mysqlConfig.username = configManager.GetString("Mysql.username", mysqlConfig.username);
			mysqlConfig.password = configManager.GetString("Mysql.password", mysqlConfig.password);
			mysqlConfig.database = configManager.GetString("Mysql.database", mysqlConfig.database);
			mysqlConfig.multiQueries = configManager.GetBool("Mysql.multiQueries", mysqlConfig.multiQueries);

			// 不能用 GetSize:它把 <= 0 当"回退到核数"
			const int poolInitialSize = configManager.GetInt("Mysql.poolInitialSize", static_cast<int>(mysqlConfig.poolInitialSize));
			mysqlConfig.poolInitialSize = (poolInitialSize > 0) ? static_cast<std::size_t>(poolInitialSize) : 1;

			const int poolMaxSize = configManager.GetInt("Mysql.poolMaxSize", static_cast<int>(mysqlConfig.poolMaxSize));
			mysqlConfig.poolMaxSize = (poolMaxSize > 0) ? static_cast<std::size_t>(poolMaxSize) : 1;

			mysqlConfig.connectTimeoutSeconds = configManager.GetInt("Mysql.connectTimeoutSeconds", mysqlConfig.connectTimeoutSeconds);
			mysqlConfig.pingIntervalSeconds = configManager.GetInt("Mysql.pingIntervalSeconds", mysqlConfig.pingIntervalSeconds);
			mysqlConfig.pingTimeoutSeconds = configManager.GetInt("Mysql.pingTimeoutSeconds", mysqlConfig.pingTimeoutSeconds);

		}

	}

}