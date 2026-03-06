#include <iostream>
#include <string>
#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"
#include "WebRTCMysqlManagerPools.h"
#include "ConfigManager.h"

#include "Utils.h"

int main() {

#ifdef _WIN32

    SetConsoleOutputCP(CP_UTF8);

    SetConsoleCP(CP_UTF8);
#endif

    ConfigManager::Instance().Load("config.ini", ConfigManager::Format::Ini);

    initLogger();

    setConsoleOutputLevels(ConfigManager::Instance().GetInt("WebRTCSignalServer.DEBUG")
        , ConfigManager::Instance().GetInt("WebRTCSignalServer.INFO")
        , ConfigManager::Instance().GetInt("WebRTCSignalServer.WARN")
        , ConfigManager::Instance().GetInt("WebRTCSignalServer.ERROR"));

    hope::core::WebRTCSignalSocket::initSslContext();

	boost::asio::io_context ioContext;

	size_t port = ConfigManager::Instance().GetInt("WebRTCSignalServer.port");

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

	hope::core::WebRTCSignalServer webrtcSignalServer(ioContext, port);

    webrtcSignalServer.run();

    hope::mysql::WebRTCMysqlManagerPools::getInstance();

    boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);

    signals.async_wait([&ioContext,&webrtcSignalServer,&work](const boost::system::error_code& error, int signal) {

		webrtcSignalServer.shutdown();

		work.reset();

        ioContext.stop();

        });

    ioContext.run();
    
    return 0;

}