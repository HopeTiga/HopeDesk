#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <atomic>
#include <boost/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "WebRTCSignalSocket.h"
#include "Utils.h"

enum class WebRTCRequestState {
    REGISTER = 0,
    REQUEST = 1,
	RESTART = 2,
	STOPREMOTE = 3,
    CLOSE = 4,
};


class WebRTCSignalServer {
public:
    WebRTCSignalServer(boost::asio::io_context & ioContext, size_t port = 8088, size_t hashValue = 1024);

    ~WebRTCSignalServer();  // 🔧 新增析构函数声明

    // 禁止拷贝和赋值
    WebRTCSignalServer(const WebRTCSignalServer&) = delete;

    WebRTCSignalServer& operator=(const WebRTCSignalServer&) = delete;

	void handleMessage(boost::json::object message,std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket);

	void run();

    void stop();

    // 新增：优雅关闭方法
    void shutdown();


private:

    void removeConnection(const std::string& userId);

    void clearAllConnections();

    size_t getBucketIndex(const std::string& userId);

    // 辅助函数：注册连接 (假设连接 ID 由客户端在 "register" 消息中提供)
    void registerConnection(const std::string& userId, std::shared_ptr<WebRTCSignalSocket> socket);

private:

    size_t hashValue;

    std::vector<std::unordered_map<std::string, std::shared_ptr<WebRTCSignalSocket>>> webrtcSignalSocketBuckets;

    std::vector<std::mutex> webSocketHashMutexs;

    std::atomic<bool> isShuttingDown{ false };  // 🔧 新增：关闭标志

	boost::asio::io_context& ioContext;

	boost::asio::ip::tcp::acceptor acceptor;

    size_t port;
};