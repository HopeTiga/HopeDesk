#include <iostream>
#include <string>
#include "WebRTCSignalServer.h"
#include "WebRTCLogicSystem.h"
#include "WebRTCMysqlManager.h"
#include "Utils.h"

int main() {
	
	boost::asio::io_context ioContext;

	size_t port = 8088;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

	Hope::WebRTCSignalServer webrtcSignalServer(ioContext, port);

    webrtcSignalServer.run();

    ioContext.run();
    
    return 0;

}