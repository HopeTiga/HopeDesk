#include "WebRTCSignalServer.h"
#include <iostream>
#include <string>
#include "Utils.h"
#include "rtc/configuration.hpp"

int main() {

    // 配置 WebSocket 服务器
    rtc::WebSocketServerConfiguration config = {
        .port = 8088,
        .enableTls = false
    };
	
	WebRTCSignalServer webrtcSignalServer(config);

    LOG_INFO("WebRTCSignalServer running on port 8088. Press 'q' to quit.");

    // 主循环
    std::string command;
    while (true) {
        std::cin >> command;
        if (command == "q") {
            break;
        }
    }

    return 0;

}