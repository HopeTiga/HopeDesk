#include <iostream>
#include <string>
#include <clocale>
#include <mimalloc/mimalloc-new-delete.h>
#include "utils/MimallocConfig.h"
#include "utils/LoggerConfig.h"
#include "ssl/Ssl.h"
#include "executor/SchedulerConfig.h"
#include "executor/SchedulerContext.h"
#include "signal/WebrtcSignalConfig.h"
#include "signal/WebrtcSignalServer.h"
#include "rpc/CoroRpcConfig.h"
#include "rpc/Rpc.h"
#include "utils/ConfigManager.h"
#include "storage/MysqlConfig.h"
#include "storage/RedisConfig.h"
#include "utils/Utils.h"

int main() {

    mi_version();

#ifdef _WIN32

    SetConsoleOutputCP(CP_UTF8);

    SetConsoleCP(CP_UTF8);

#else

    std::setlocale(LC_ALL, "C.UTF-8");

#endif

    hope::utils::ConfigManager& configManager = hope::utils::ConfigManager::Instance();

    configManager.Load("config.ini", hope::utils::ConfigManager::Format::Ini);

    hope::utils::MimallocConfig mimallocConfig;
    hope::utils::loadMimallocConfig(mimallocConfig, configManager);
    hope::utils::applyMimallocConfig(mimallocConfig);

    hope::utils::LoggerConfig loggerConfig;
    hope::utils::loadLoggerConfig(loggerConfig, configManager);
    hope::utils::applyLoggerConfig(loggerConfig);

    hope::executor::SchedulerConfig schedulerConfig;
    hope::executor::loadSchedulerConfig(schedulerConfig, configManager);
    hope::executor::SchedulerContext::init(schedulerConfig);

    hope::signal::WebrtcSignalConfig webrtcSignalConfig;
    hope::signal::loadWebrtcSignalConfig(webrtcSignalConfig, configManager);
    webrtcSignalConfig.threadSize = schedulerConfig.threadSize;  

    hope::rpc::loadCoroRpcConfig(webrtcSignalConfig.coroRpcServerConfig, configManager);

    hope::storage::loadMysqlConfig(webrtcSignalConfig.mysqlConfig, configManager);

    hope::storage::loadRedisConfig(webrtcSignalConfig.redisConfig, configManager);

    initSslContext(webrtcSignalConfig.certificateFile, webrtcSignalConfig.privateKeyFile);

    boost::asio::io_context ioContext{ 1 };

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    std::shared_ptr<hope::signal::WebrtcSignalServer> webrtcSignalServer = std::make_shared<hope::signal::WebrtcSignalServer>(ioContext, webrtcSignalConfig);

    initCoroRpcHandleInterface(webrtcSignalServer);

    if (!webrtcSignalServer->asyncEvent()) {

        LOG_INFO("WebrtcSignalServer AsyncEvent Failed");

        return -1;

    }

    boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);

    signals.async_wait([&ioContext, webrtcSignalServer = webrtcSignalServer->shared_from_this(), &work](const boost::system::error_code& error, int signal) {

        webrtcSignalServer->closeEvent();

        work.reset();

        ioContext.stop();

        closeLogger();

        });

    ioContext.run();

    return 0;

}