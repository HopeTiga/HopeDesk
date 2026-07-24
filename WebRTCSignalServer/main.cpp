#include <iostream>
#include <string>
#include <clocale>
#include "Ssl.h"
#include "signal/WebRTCSignalServer.h"
#include "utils/ConfigManager.h"
#include "mysql/MysqlConfig.h"
#include "iocp/AsioProactors.h"
#include "utils/Utils.h"

int main() {

#ifdef _WIN32

    SetConsoleOutputCP(CP_UTF8);

    SetConsoleCP(CP_UTF8);

#else

    std::setlocale(LC_ALL, "C.UTF-8");

#endif

    hope::utils::ConfigManager& configManager = hope::utils::ConfigManager::Instance();

    configManager.Load("config.ini", hope::utils::ConfigManager::Format::Ini);

    initLogger();

    setConsoleOutputLevels(configManager.GetInt("WebRTCSignalServer.DEBUG")
        , configManager.GetInt("WebRTCSignalServer.INFO")
        , configManager.GetInt("WebRTCSignalServer.WARN")
        , configManager.GetInt("WebRTCSignalServer.ERROR"));

    std::string certificateFile = configManager.GetString("WebRTCSignalServer.certificateFile");

    std::string privateKeyFile = configManager.GetString("WebRTCSignalServer.privateKeyFile");

    initSslContext(certificateFile, privateKeyFile);

    hope::signal::WebRTCSignalConfig webrtcSignalConfig;
    webrtcSignalConfig.signalPort = configManager.GetInt("WebRTCSignalServer.port");
    webrtcSignalConfig.enableHttp = configManager.GetInt("WebRTCSignalServer.enableHttp");
    webrtcSignalConfig.httpPort = configManager.GetInt("WebRTCSignalServer.httpPort");
    webrtcSignalConfig.enablePublicPort = configManager.GetInt("WebRTCSignalServer.enablePublicPort");
    webrtcSignalConfig.threadSize = configManager.GetSize("WebRTCSignalServer.size");
    webrtcSignalConfig.overload = configManager.GetInt("WebRTCSignalServer.overload");
    webrtcSignalConfig.threshold = configManager.GetInt("WebRTCSignalServer.threshold");
    webrtcSignalConfig.exitThreshold = configManager.GetInt("WebRTCSignalServer.exitThreshold");
    webrtcSignalConfig.asyncThreshold = configManager.GetInt("WebRTCSignalServer.asyncThreshold");
    webrtcSignalConfig.socketWaitTime = configManager.GetInt("WebRTCSignalServer.socketWaitTime");

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

    std::shared_ptr<hope::signal::WebRTCSignalServer> webrtcSignalServer = std::make_shared<hope::signal::WebRTCSignalServer>(ioContext, webrtcSignalConfig);

    webrtcSignalServer->asyncEvent();

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