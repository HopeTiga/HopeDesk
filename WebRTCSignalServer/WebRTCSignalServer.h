#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <atomic>
#include "rtc/configuration.hpp"
#include "rtc/WebSocketServer.hpp"
#include "rtc/WebSocket.hpp"
#include "rtc/PeerConnection.hpp"
#include "rtc/description.hpp"
#include "rtc/rtppacketizationconfig.hpp"
#include "rtc/h264rtppacketizer.hpp"
#include "rtc/RtcpSrReporter.hpp"
#include "rtc/RtcpNackResponder.hpp"
#include "Utils.h"

#define RTC_ICE_CONFIGURATION_H
#define RTC_ENABLE_WEBSOCKET
#define RTC_FRAMEINFO_H

enum class WebRTCRequestState {
    REGISTER = 0,
    REQUEST = 1,
	RESTART = 2,
	STOPREMOTE = 3,
    CLOSE = 4,
};

struct WebRTCConnection {

    std::string connectionID;

    std::string accountID;

    size_t hashIndex;

    std::shared_ptr<rtc::WebSocket> webSocket;

};

class WebRTCSignalServer {
public:
    WebRTCSignalServer(rtc::WebSocketServerConfiguration config, size_t hashValue = 1024);

    ~WebRTCSignalServer();  // 🔧 新增析构函数声明


    // 禁止拷贝和赋值
    WebRTCSignalServer(const WebRTCSignalServer&) = delete;

    WebRTCSignalServer& operator=(const WebRTCSignalServer&) = delete;

    // 新增：优雅关闭方法
    void shutdown();


private:
    void removeConnection(std::shared_ptr<WebRTCConnection> connection);

    void clearAllConnections();

private:
    std::shared_ptr<rtc::WebSocketServer> webSocketServer;
    size_t hashValue;
    std::vector<std::unordered_map<std::string, std::shared_ptr<WebRTCConnection>>> webSocketPeerConnections;
    std::vector<std::mutex> webSocketHashMutexs;

    std::atomic<bool> isShuttingDown{ false };  // 🔧 新增：关闭标志
};