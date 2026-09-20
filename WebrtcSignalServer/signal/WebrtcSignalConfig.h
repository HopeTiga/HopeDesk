#pragma once

#include <string>
#include <thread>

#include "../rpc/CoroRpcConfig.h"
#include "../storage/MysqlConfig.h"
#include "../storage/RedisConfig.h"
#include "../utils/ConfigManager.h"

namespace hope {

    namespace signal {

        struct WebrtcSignalConfig {

            size_t signalPort = 8088;

            size_t enableHttp = 0;

            size_t httpPort = 9099;

            size_t enablePublicPort = 1;

            size_t threadSize = std::thread::hardware_concurrency();

            size_t enableRpc = 0;

            std::string certificateFile{"server.crt"};

            std::string privateKeyFile{"server.key"};

            hope::rpc::CoroRpcServerConfig coroRpcServerConfig;

            int overload = 256;

            int threshold = 256;

            int exitThreshold = 128;

            int asyncThreshold = 32;

            int maxTlsHandShakeTime = 10000;

            int maxTlsHttpHandShakeTime = 10000;

            int maxHttpKeepAliveTime = 300;

            hope::storage::MysqlConfig mysqlConfig;

            hope::storage::RedisConfig redisConfig;

        };

        inline void loadWebrtcSignalConfig(WebrtcSignalConfig& webrtcSignalConfig, const hope::utils::ConfigManager& configManager) {

            webrtcSignalConfig.signalPort = static_cast<size_t>(configManager.GetInt("WebrtcSignalServer.port", static_cast<int>(webrtcSignalConfig.signalPort)));

            webrtcSignalConfig.enableHttp = static_cast<size_t>(configManager.GetInt("WebrtcSignalServer.enableHttp", static_cast<int>(webrtcSignalConfig.enableHttp)));

            webrtcSignalConfig.httpPort = static_cast<size_t>(configManager.GetInt("WebrtcSignalServer.httpPort", static_cast<int>(webrtcSignalConfig.httpPort)));

            webrtcSignalConfig.enablePublicPort = static_cast<size_t>(configManager.GetInt("WebrtcSignalServer.enablePublicPort", static_cast<int>(webrtcSignalConfig.enablePublicPort)));

            webrtcSignalConfig.certificateFile = configManager.GetString("WebrtcSignalServer.certificateFile", webrtcSignalConfig.certificateFile);

            webrtcSignalConfig.privateKeyFile = configManager.GetString("WebrtcSignalServer.privateKeyFile", webrtcSignalConfig.privateKeyFile);

            webrtcSignalConfig.overload = configManager.GetInt("WebrtcSignalServer.overload", webrtcSignalConfig.overload);

            webrtcSignalConfig.threshold = configManager.GetInt("WebrtcSignalServer.threshold", webrtcSignalConfig.threshold);

            webrtcSignalConfig.exitThreshold = configManager.GetInt("WebrtcSignalServer.exitThreshold", webrtcSignalConfig.exitThreshold);

            webrtcSignalConfig.asyncThreshold = configManager.GetInt("WebrtcSignalServer.asyncThreshold", webrtcSignalConfig.asyncThreshold);

            webrtcSignalConfig.maxTlsHandShakeTime = configManager.GetInt("WebrtcSignalServer.maxTlsHandShakeTime", webrtcSignalConfig.maxTlsHandShakeTime);

            webrtcSignalConfig.maxTlsHttpHandShakeTime = configManager.GetInt("WebrtcSignalServer.maxTlsHttpHandShakeTime", webrtcSignalConfig.maxTlsHttpHandShakeTime);

            webrtcSignalConfig.maxHttpKeepAliveTime = configManager.GetInt("WebrtcSignalServer.maxHttpKeepAliveTime", webrtcSignalConfig.maxHttpKeepAliveTime);

            webrtcSignalConfig.enableRpc = configManager.GetInt("CoroRpc.enableRpc", 0) != 0;


        }

    }

}
