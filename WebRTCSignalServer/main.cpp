#include <iostream>
#include <string>
#include "WebRTCSignalServer.h"
#include "ConfigManager.h"

#include "Utils.h"

int main() {

#ifdef _WIN32
    // 设置控制台输出编码为UTF-8
    SetConsoleOutputCP(CP_UTF8);
    // 可选：设置输入编码也为UTF-8
    SetConsoleCP(CP_UTF8);
#endif

    ConfigManager::Instance().Load("config.ini", ConfigManager::Format::Ini);

    boost::asio::io_context ioContext;

    size_t port = ConfigManager::Instance().GetInt("WebRTCSiganlServer.port");

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    hope::core::WebRTCSignalServer webrtcSignalServer(ioContext, port);

    webrtcSignalServer.run();

    ioContext.run();

    return 0;

}