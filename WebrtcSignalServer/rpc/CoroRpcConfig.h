#pragma once

#include <cstddef>
#include <string>

#include "../utils/ConfigManager.h"

namespace hope {

	namespace rpc {

		struct CoroRpcServerConfig {

			size_t port = 10011;

			size_t threadSize = 2;

			bool enableSsl = false;

			std::string basePath{"."};

			std::string certFile;

			std::string keyFile;

			std::string caCertFile; // 单向认证为空

			bool enableClientVerify = false;

			bool enableDoubleSsl = false; // 双向认证

			std::string clientCertFile;

			std::string clientKeyFile;

		};

		inline void loadCoroRpcConfig(CoroRpcServerConfig& coroRpcServerConfig, const hope::utils::ConfigManager& configManager) {

			coroRpcServerConfig.port = configManager.GetSize("CoroRpc.port", coroRpcServerConfig.port);

			coroRpcServerConfig.threadSize = configManager.GetSize("CoroRpc.threadSize", coroRpcServerConfig.threadSize);

			coroRpcServerConfig.enableSsl = configManager.GetBool("CoroRpc.enableSsl", coroRpcServerConfig.enableSsl);

			coroRpcServerConfig.basePath = configManager.GetString("CoroRpc.basePath", coroRpcServerConfig.basePath);

			coroRpcServerConfig.certFile = configManager.GetString("CoroRpc.certFile", coroRpcServerConfig.certFile);

			coroRpcServerConfig.keyFile = configManager.GetString("CoroRpc.keyFile", coroRpcServerConfig.keyFile);

			coroRpcServerConfig.caCertFile = configManager.GetString("CoroRpc.caCertFile", coroRpcServerConfig.caCertFile);

			coroRpcServerConfig.enableClientVerify = configManager.GetBool("CoroRpc.enableClientVerify", coroRpcServerConfig.enableClientVerify);

			coroRpcServerConfig.enableDoubleSsl = configManager.GetBool("CoroRpc.enableDoubleSsl", coroRpcServerConfig.enableDoubleSsl);

			coroRpcServerConfig.clientCertFile = configManager.GetString("CoroRpc.clientCertFile", coroRpcServerConfig.clientCertFile);

			coroRpcServerConfig.clientKeyFile = configManager.GetString("CoroRpc.clientKeyFile", coroRpcServerConfig.clientKeyFile);

		}

	}

}
