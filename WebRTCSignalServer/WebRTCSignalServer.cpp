#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "AsioProactors.h"
#include "Logger.h"


WebRTCSignalServer::WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port , size_t hashValue)
	: ioContext(ioContext)
	, port(port)
    , hashValue(hashValue)
    , webSocketHashMutexs(hashValue)
    , webrtcSignalSocketBuckets(hashValue)
	, acceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), port))
{
    
}

void WebRTCSignalServer::handleMessage(boost::json::object message, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket)
{

	LOG_DEBUG("收到消息: %s", boost::json::serialize(message).c_str());

    // 检查 requestType 字段是否存在
    if (!message.contains("requestType")) {
        // [可选] 发送错误响应给客户端
        LOG_WARNING("收到缺少 requestType 字段的无效消息.");
        return;
    }

    // 安全地提取 requestType 并转换为枚举
    int64_t requestTypeValue = message["requestType"].as_int64();
    WebRTCRequestState requestType = WebRTCRequestState(requestTypeValue);

    switch (requestType) {

    case WebRTCRequestState::REGISTER: {
        // --------------------------------------------------
        // A. 注册逻辑
        // --------------------------------------------------
        if (!message.contains("accountID")) {
            LOG_WARNING("REGISTER 消息缺少 accountID.");
            break;
        }
        std::string accountID = message["accountID"].as_string().c_str();

        // 1. 计算哈希，并存储到连接对象
        size_t hashSize = std::hash<std::string>{}(accountID) % this->hashValue;
        webrtcSignalSocket->setAccountID(accountID); // 假设 setAccountID 存在
        webrtcSignalSocket->setHashIndex(hashSize);  // 假设 setHashIndex 存在
        webrtcSignalSocket->setRegistered(true);
        // 2. 注册到全局连接表 (使用哈希锁)
        {
            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);
   
            webrtcSignalSocketBuckets[hashSize][accountID] = webrtcSignalSocket;
        }

        // 3. 响应客户端 (使用同步 send - 考虑改为异步 writerAsync)
        boost::json::object response;
        response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REGISTER);
        response["state"] = 200;
        response["message"] = "register successful";

        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 使用 writerAsync 替代同步 send

        LOG_INFO("用户注册成功: %s (哈希桶: %zu)", accountID.c_str(), hashSize);
        break;
    }

    case WebRTCRequestState::REQUEST:
    case WebRTCRequestState::RESTART:
    case WebRTCRequestState::STOPREMOTE: {

        if (!message.contains("accountID") || !message.contains("targetID")) {
            LOG_WARNING("转发消息缺少 accountID 或 targetID.");
            break;
        }

        std::string accountID = message["accountID"].as_string().c_str(); // 发送方 ID
        std::string targetID = message["targetID"].as_string().c_str();   // 目标方 ID

        size_t hashSize = std::hash<std::string>{}(targetID) % this->hashValue;
        std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

        // 1. 查找目标连接 (使用哈希锁)
        {
            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);
            auto it = webrtcSignalSocketBuckets[hashSize].find(targetID);
            if (it != webrtcSignalSocketBuckets[hashSize].end()) {
                targetSocket = it->second;
            }
        }

        // 2. 处理目标未找到 (404)
        if (!targetSocket) {
            boost::json::object response;
            response["requestType"] = requestTypeValue;
            response["state"] = 404;
            response["message"] = "targetID is not register";
            webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

            LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",
                targetID.c_str(), accountID.c_str(),
                (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));
            break;
        }

        // 3. 转发消息
        boost::json::object forwardMessage = message; // 复制原始消息体
        forwardMessage["state"] = 200;
        forwardMessage["message"] = "WebRTCSignalServer forward";
        targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

        LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",
            accountID.c_str(), targetID.c_str(),
            (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));
        break;
    }

    case WebRTCRequestState::CLOSE: {
        std::string accountID = webrtcSignalSocket->getAccountID();
        if (!accountID.empty()) {
            this->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
        }
        webrtcSignalSocket->stop(); // 关闭 socket 实例
        LOG_INFO("收到用户 %s 的 CLOSE 请求，连接已停止", accountID.c_str());
        break;
    }

    default: {
        LOG_WARNING("收到未知的请求类型: %lld", requestTypeValue);
        break;
    }
    }
}

void WebRTCSignalServer::run() {
    // 启动服务器逻辑（监听端口，接受连接等）
    LOG_INFO("WebRTC信令服务器正在运行，监听端口: %zu", port);
    // 这里可以添加更多的启动逻辑
    boost::asio::co_spawn(ioContext, [this]()->boost::asio::awaitable<void> {

        while (!isShuttingDown.load()) {
            try {

                std::pair<int, boost::asio::io_context&> pair = AsioProactors::getInstance()->getIoComplatePorts();

                std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<WebRTCSignalSocket>(pair.second,pair.first,*this);

                co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                boost::asio::ip::tcp::endpoint remoteEndpoint = webrtcSignalSocket->getSocket().remote_endpoint();

                std::string clientIP = remoteEndpoint.address().to_string();

                unsigned short clientPort = remoteEndpoint.port();

                Logger::getInstance()->info("new Connection from Address: "+std::string(clientIP.c_str()) + ":" + std::to_string(clientPort) );

                boost::asio::co_spawn(pair.second, [this, sharedWebrtcSignalSocket =  webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                    co_await sharedWebrtcSignalSocket->handShake();

                    sharedWebrtcSignalSocket->start();

                    }, boost::asio::detached);

            }
            catch (const std::exception& e) {
                if (!isShuttingDown.load()) {
                    LOG_ERROR("接受连接时出错: %s", e.what());
                }
            }
        }
        }, [this](std::exception_ptr p) {});

}


WebRTCSignalServer::~WebRTCSignalServer() {
    LOG_INFO("WebRTC信令服务器开始关闭...");
    shutdown();
    LOG_INFO("WebRTC信令服务器已完全关闭");
}

void WebRTCSignalServer::shutdown() {
    LOG_INFO("设置关闭标志，停止接受新连接");
    // 设置关闭标志，防止新连接
    isShuttingDown.store(true);

    // 清理所有连接
    clearAllConnections();

}

void WebRTCSignalServer::clearAllConnections() {
    LOG_INFO("开始清理所有连接...");

    size_t totalConnections = 0;

    // 遍历所有哈希桶
    for (size_t i = 0; i < hashValue; ++i) {
        std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[i]);

        // 关闭所有WebSocket连接
        for (auto& pair : webrtcSignalSocketBuckets[i]) {
            if (pair.second && pair.second->getWebSocket().is_open()) {
                try {
                    pair.second->getWebSocket().next_layer().cancel();
                    pair.second->getWebSocket().close(boost::beast::websocket::close_code::normal);
                    totalConnections++;
                    LOG_DEBUG("关闭连接: %s", pair.first.c_str());
                }
                catch (const std::exception& e) {
                    LOG_ERROR("关闭连接 %s 时出错: %s", pair.first.c_str(), e.what());
                }
            }
        }

        // 清空连接映射
        webrtcSignalSocketBuckets[i].clear();
    }

    LOG_INFO("连接清理完成 - 共清理 %zu 个连接", totalConnections);
}

size_t WebRTCSignalServer::getBucketIndex(const std::string& userId) {
    // 假设 std::hash<std::string> 是可用的
    return std::hash<std::string>{}(userId) % hashValue;
}

// 辅助函数：注册连接 (假设连接 ID 由客户端在 "register" 消息中提供)
void WebRTCSignalServer::registerConnection(const std::string& userId, std::shared_ptr<WebRTCSignalSocket> socket) {
    size_t index = getBucketIndex(userId);
    std::lock_guard<std::mutex> lock(webSocketHashMutexs[index]);

    // 假设 WebRTCSignalSocket 实例需要存储其注册 ID，这里暂不实现
    // ⚠️ 如果 ID 已经存在，应该先关闭旧连接或处理重连逻辑

    webrtcSignalSocketBuckets[index][userId] = socket;
    LOG_INFO("注册新用户: %s, 存储在桶 %zu", userId.c_str(), index);
}

// 辅助函数：移除连接
void WebRTCSignalServer::removeConnection(const std::string& userId) {
    if (userId.empty()) return;

    size_t index = getBucketIndex(userId);
    std::lock_guard<std::mutex> lock(webSocketHashMutexs[index]);

    if (webrtcSignalSocketBuckets[index].erase(userId) > 0) {
        LOG_INFO("移除用户连接: %s", userId.c_str());
    }
}