#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/ssl/context.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/logger.hpp>

#include "../utils/ConfigManager.h"

namespace hope {

	namespace storage {

		struct RedisConfig {

			std::string host{"127.0.0.1"};

			std::string port{"6379"};

			std::string username{"default"};

			std::string password;

			std::string clientName{"Boost.Redis"};

			int databaseIndex{0};

			bool useSsl{false};               // 关掉就是明文 TCP，下面四项都不用配

			std::string caCertificateFile;

			std::string certificateFile;

			std::string privateKeyFile;

			bool verifyPeer{true};

			int connectTimeoutSeconds{10};

			int sslHandshakeTimeoutSeconds{10};

			int healthCheckIntervalSeconds{2};

			int reconnectWaitIntervalSeconds{1};

			std::size_t maxReadSize{0};       // 0 = 不限制

			std::size_t connectionSize{1};   // 每个 channel 连几条

			bool enableLog{false};

			std::string logLevel{"info"};    // disabled/emerg/alert/crit/err/warning/notice/info/debug

		};

		inline void loadRedisConfig(RedisConfig& redisConfig, const hope::utils::ConfigManager& configManager) {

			redisConfig.host = configManager.GetString("Redis.host", redisConfig.host);
			redisConfig.port = configManager.GetString("Redis.port", redisConfig.port);
			redisConfig.username = configManager.GetString("Redis.username", redisConfig.username);
			redisConfig.password = configManager.GetString("Redis.password", redisConfig.password);
			redisConfig.clientName = configManager.GetString("Redis.clientName", redisConfig.clientName);
			redisConfig.databaseIndex = configManager.GetInt("Redis.databaseIndex", redisConfig.databaseIndex);
			redisConfig.useSsl = configManager.GetBool("Redis.useSsl", redisConfig.useSsl);
			redisConfig.caCertificateFile = configManager.GetString("Redis.caCertificateFile", redisConfig.caCertificateFile);
			redisConfig.certificateFile = configManager.GetString("Redis.certificateFile", redisConfig.certificateFile);
			redisConfig.privateKeyFile = configManager.GetString("Redis.privateKeyFile", redisConfig.privateKeyFile);
			redisConfig.verifyPeer = configManager.GetBool("Redis.verifyPeer", redisConfig.verifyPeer);
			redisConfig.connectTimeoutSeconds = configManager.GetInt("Redis.connectTimeoutSeconds", redisConfig.connectTimeoutSeconds);
			redisConfig.sslHandshakeTimeoutSeconds = configManager.GetInt("Redis.sslHandshakeTimeoutSeconds", redisConfig.sslHandshakeTimeoutSeconds);
			redisConfig.healthCheckIntervalSeconds = configManager.GetInt("Redis.healthCheckIntervalSeconds", redisConfig.healthCheckIntervalSeconds);
			redisConfig.reconnectWaitIntervalSeconds = configManager.GetInt("Redis.reconnectWaitIntervalSeconds", redisConfig.reconnectWaitIntervalSeconds);

			// 不能用 GetSize:它把 <= 0 当"回退到核数"
			int maxReadSize = configManager.GetInt("Redis.maxReadSize", 0);
			redisConfig.maxReadSize = (maxReadSize > 0) ? static_cast<std::size_t>(maxReadSize) : 0;

			int connectionSize = configManager.GetInt("Redis.connectionSize", static_cast<int>(redisConfig.connectionSize));
			redisConfig.connectionSize = (connectionSize > 0) ? static_cast<std::size_t>(connectionSize) : 1;

			redisConfig.enableLog = configManager.GetBool("Redis.enableLog", redisConfig.enableLog);
			redisConfig.logLevel = configManager.GetString("Redis.logLevel", redisConfig.logLevel);

		}

		inline boost::redis::logger makeRedisLogger(const RedisConfig& redisConfig) {

			if (!redisConfig.enableLog) {

				return boost::redis::logger{ boost::redis::logger::level::disabled };

			}

			static const std::pair<std::string_view, boost::redis::logger::level> levelNames[] = {

				{"emerg", boost::redis::logger::level::emerg},
				{"alert", boost::redis::logger::level::alert},
				{"crit", boost::redis::logger::level::crit},
				{"err", boost::redis::logger::level::err},
				{"warning", boost::redis::logger::level::warning},
				{"notice", boost::redis::logger::level::notice},
				{"info", boost::redis::logger::level::info},
				{"debug", boost::redis::logger::level::debug},

			};

			// 名字不认识就当 info，不要静默成 disabled——配置写错了要看得见日志。
			for (const std::pair<std::string_view, boost::redis::logger::level>& levelName : levelNames) {

				if (redisConfig.logLevel == levelName.first) {

					return boost::redis::logger{ levelName.second };

				}

			}

			return boost::redis::logger{ boost::redis::logger::level::info };

		}

		inline boost::redis::config makeBoostRedisConfig(const RedisConfig& redisConfig) {

			boost::redis::config boostRedisConfig;

			boostRedisConfig.addr.host = redisConfig.host;
			boostRedisConfig.addr.port = redisConfig.port;
			boostRedisConfig.username = redisConfig.username;
			boostRedisConfig.password = redisConfig.password;
			boostRedisConfig.clientname = redisConfig.clientName;
			boostRedisConfig.database_index = redisConfig.databaseIndex;
			boostRedisConfig.use_ssl = redisConfig.useSsl;
			boostRedisConfig.connect_timeout = std::chrono::seconds(redisConfig.connectTimeoutSeconds);
			boostRedisConfig.ssl_handshake_timeout = std::chrono::seconds(redisConfig.sslHandshakeTimeoutSeconds);
			boostRedisConfig.health_check_interval = std::chrono::seconds(redisConfig.healthCheckIntervalSeconds);
			boostRedisConfig.reconnect_wait_interval = std::chrono::seconds(redisConfig.reconnectWaitIntervalSeconds);

			if (redisConfig.maxReadSize > 0) {

				boostRedisConfig.max_read_size = redisConfig.maxReadSize;

			}

			return boostRedisConfig;

		}

		// context 只能从 connection 构造函数进去（config.use_ssl 才是握不握手的开关），
		// useSsl 为假时这个 context 建了不会用到。
		inline boost::asio::ssl::context makeRedisSslContext(const RedisConfig& redisConfig) {

			boost::asio::ssl::context sslContext{ boost::asio::ssl::context::tls_client };

			if (!redisConfig.useSsl) {

				return sslContext;

			}

			if (redisConfig.verifyPeer) {

				sslContext.set_verify_mode(boost::asio::ssl::verify_peer);

				if (!redisConfig.caCertificateFile.empty()) {

					sslContext.load_verify_file(redisConfig.caCertificateFile);

				}

			}
			else {

				sslContext.set_verify_mode(boost::asio::ssl::verify_none);

			}

			if (!redisConfig.certificateFile.empty()) {

				sslContext.use_certificate_chain_file(redisConfig.certificateFile);

			}

			if (!redisConfig.privateKeyFile.empty()) {

				sslContext.use_private_key_file(redisConfig.privateKeyFile, boost::asio::ssl::context::pem);

			}

			return sslContext;

		}

	}

}
