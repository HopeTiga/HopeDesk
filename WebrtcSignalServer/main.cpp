#include <iostream>
#include <string>
#include <clocale>
#include <mimalloc/mimalloc-new-delete.h>
#include "utils/MimallocConfig.h"
#include "ssl/Ssl.h"
#include "iocp/AsioProactors.h"
#include "signal/WebrtcSignalServer.h"
#include "rpc/Rpc.h"
#include "utils/ConfigManager.h"
#include "mysql/MysqlConfig.h"
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

    setLoggerAsyncConfig(configManager.GetInt("Logger.queueSize", 8192)
        , configManager.GetInt("Logger.threadCount", 1));

    initLogger();

    setConsoleOutputLevels(configManager.GetInt("Logger.DEBUG")
        , configManager.GetInt("Logger.INFO")
        , configManager.GetInt("Logger.WARN")
        , configManager.GetInt("Logger.ERROR"));

    setFileLoggingConfig(configManager.GetInt("Logger.logToFile", 1)
        , configManager.GetString("Logger.logDirectory", "logs").c_str()
        , configManager.GetInt("Logger.maxFileSizeMB", 10)
        , configManager.GetInt("Logger.maxFiles", 5));

    std::string certificateFile = configManager.GetString("WebrtcSignalServer.certificateFile");

    std::string privateKeyFile = configManager.GetString("WebrtcSignalServer.privateKeyFile");

    initSslContext(certificateFile, privateKeyFile);

    hope::signal::WebrtcSignalConfig webrtcSignalConfig;
    webrtcSignalConfig.signalPort = configManager.GetInt("WebrtcSignalServer.port");
    webrtcSignalConfig.enableHttp = configManager.GetInt("WebrtcSignalServer.enableHttp");
    webrtcSignalConfig.httpPort = configManager.GetInt("WebrtcSignalServer.httpPort");
    webrtcSignalConfig.enablePublicPort = configManager.GetInt("WebrtcSignalServer.enablePublicPort");
    webrtcSignalConfig.threadSize = configManager.GetSize("WebrtcSignalServer.size");
    webrtcSignalConfig.overload = configManager.GetInt("WebrtcSignalServer.overload");
    webrtcSignalConfig.threshold = configManager.GetInt("WebrtcSignalServer.threshold");
    webrtcSignalConfig.exitThreshold = configManager.GetInt("WebrtcSignalServer.exitThreshold");
    webrtcSignalConfig.asyncThreshold = configManager.GetInt("WebrtcSignalServer.asyncThreshold");
    webrtcSignalConfig.maxTlsHandShakeTime = configManager.GetInt("WebrtcSignalServer.maxTlsHandShakeTime");

    webrtcSignalConfig.maxTlsHttpHandShakeTime = configManager.GetInt("WebrtcSignalServer.maxTlsHttpHandShakeTime", 10000);

    webrtcSignalConfig.maxHttpKeepAliveTime = configManager.GetInt("WebrtcSignalServer.maxHttpKeepAliveTime", 300);

    webrtcSignalConfig.enableRpc = configManager.GetInt("CoroRpc.enableRpc", 0) != 0;
    hope::rpc::CoroRpcServerConfig& coroRpcConfig = webrtcSignalConfig.coroRpcServerConfig;
    coroRpcConfig.port = static_cast<size_t>(configManager.GetInt("CoroRpc.port", 9001));
    coroRpcConfig.threadSize = static_cast<size_t>(configManager.GetInt("CoroRpc.threadSize", 4));
    coroRpcConfig.enableSsl = configManager.GetInt("CoroRpc.enableSsl", 0) != 0;
    coroRpcConfig.basePath = configManager.GetString("CoroRpc.basePath", ".");
    coroRpcConfig.certFile = configManager.GetString("CoroRpc.certFile");
    coroRpcConfig.keyFile = configManager.GetString("CoroRpc.keyFile");
    coroRpcConfig.caCertFile = configManager.GetString("CoroRpc.caCertFile");
    coroRpcConfig.enableClientVerify = configManager.GetInt("CoroRpc.enableClientVerify", 0) != 0;
    coroRpcConfig.enableDoubleSsl = configManager.GetInt("CoroRpc.enableDoubleSsl", 0) != 0;
    coroRpcConfig.clientCertFile = configManager.GetString("CoroRpc.clientCertFile");
    coroRpcConfig.clientKeyFile = configManager.GetString("CoroRpc.clientKeyFile");

    if (webrtcSignalConfig.threadSize <= 0) webrtcSignalConfig.threadSize = std::thread::hardware_concurrency();

    hope::mysql::globalMysqlConfig.host = configManager.GetString("Mysql.host");
    hope::mysql::globalMysqlConfig.port = static_cast<unsigned short>(configManager.GetInt("Mysql.port"));
    hope::mysql::globalMysqlConfig.username = configManager.GetString("Mysql.username");
    hope::mysql::globalMysqlConfig.password = configManager.GetString("Mysql.password");
    hope::mysql::globalMysqlConfig.database = configManager.GetString("Mysql.database");
    hope::mysql::globalMysqlConfig.multiQueries = configManager.GetInt("Mysql.multiQueries", 0) != 0;
    hope::mysql::globalMysqlConfig.poolInitialSize = static_cast<std::size_t>(configManager.GetInt("Mysql.poolInitialSize", 2));
    hope::mysql::globalMysqlConfig.poolMaxSize = static_cast<std::size_t>(configManager.GetInt("Mysql.poolMaxSize", 16));
    hope::mysql::globalMysqlConfig.connectTimeoutSeconds = configManager.GetInt("Mysql.connectTimeoutSeconds", 20);
    hope::mysql::globalMysqlConfig.pingIntervalSeconds = configManager.GetInt("Mysql.pingIntervalSeconds", 3600);
    hope::mysql::globalMysqlConfig.pingTimeoutSeconds = configManager.GetInt("Mysql.pingTimeoutSeconds", 10);

    hope::iocp::AsioProactors::init(webrtcSignalConfig.threadSize);

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