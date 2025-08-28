#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp> 
#include <boost/uuid/uuid_io.hpp>
#include <boost/json.hpp>
#include "rtc/rtc.h"

WebRTCSignalServer::WebRTCSignalServer(rtc::WebSocketServerConfiguration config, size_t hashValue)
    : webSocketServer(std::make_shared<rtc::WebSocketServer>(config))
    , hashValue(hashValue)
    , webSocketHashMutexs(hashValue)
    , webSocketPeerConnections(hashValue)
{
    webSocketServer->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
        // 检查是否正在关闭
        if (isShuttingDown.load()) {
            ws->close();
            return;
        }

        boost::uuids::random_generator generator;
        boost::uuids::uuid uuids = generator();
        std::shared_ptr<WebRTCConnection> connection = std::make_shared<WebRTCConnection>();
        connection->connectionID = boost::uuids::to_string(uuids);
        connection->webSocket = ws;

        LOG_DEBUG("新客户端连接: %s", connection->connectionID.c_str());

        ws->onMessage([this, conn = std::shared_ptr<WebRTCConnection>(connection)]
        (rtc::variant<rtc::binary, std::string> data) mutable {
                // 检查是否正在关闭
                if (isShuttingDown.load()) {
                    return;
                }

                LOG_DEBUG("json:%s", std::get<std::string>(data).c_str());

                try {

                    boost::json::object val = boost::json::parse(std::get<std::string>(data)).as_object();

                    int64_t requestType = val["requestType"].as_int64();

                    if (WebRTCRequestState(requestType) == WebRTCRequestState::REGISTER) {

                        std::string accountID = val["accountID"].as_string().c_str();

                        conn->accountID = accountID;

                        size_t hashSize = std::hash<std::string>{}(accountID) % this->hashValue;

                        conn->hashIndex = hashSize;

                        {
                            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);

                            webSocketPeerConnections[hashSize][accountID] = conn;
                        }

                        boost::json::object response;

                        response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REGISTER);

                        response["state"] = 200;

                        response["message"] = "register successful";

                        conn->webSocket->send(boost::json::serialize(response));

                        LOG_INFO("用户注册成功: %s (哈希桶: %zu)", accountID.c_str(), hashSize);
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::REQUEST) {
                        std::string accountID = val["accountID"].as_string().c_str();
                        std::string targetID = val["targetID"].as_string().c_str();

                        size_t hashSize = std::hash<std::string>{}(targetID) % this->hashValue;
                        std::shared_ptr<WebRTCConnection> targetConnection = nullptr;

                        {
                            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);
                            auto it = webSocketPeerConnections[hashSize].find(targetID);
                            if (it != webSocketPeerConnections[hashSize].end()) {
                                targetConnection = it->second;
                            }
                        }

                        if (!targetConnection) {
                            boost::json::object response;
                            response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REQUEST);
                            response["state"] = 404;
                            response["message"] = "targetID is not register";
                            conn->webSocket->send(boost::json::serialize(response));

                            LOG_WARNING("目标用户未找到: %s (来自: %s)", targetID.c_str(), accountID.c_str());
                            return;
                        }

                        // 添加发送方信息
                        boost::json::object forwardMessage = val;
                        forwardMessage["state"] = 200;
                        forwardMessage["message"] = "WebRTCSignalServer forawrd";
                        targetConnection->webSocket->send(boost::json::serialize(forwardMessage));

                        LOG_INFO("消息转发成功: %s -> %s", accountID.c_str(), targetID.c_str());
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::RESTART) {
                        std::string accountID = val["accountID"].as_string().c_str();
                        std::string targetID = val["targetID"].as_string().c_str();

                        size_t hashSize = std::hash<std::string>{}(targetID) % this->hashValue;
                        std::shared_ptr<WebRTCConnection> targetConnection = nullptr;

                        {
                            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);
                            auto it = webSocketPeerConnections[hashSize].find(targetID);
                            if (it != webSocketPeerConnections[hashSize].end()) {
                                targetConnection = it->second;
                            }
                        }

                        if (!targetConnection) {
                            boost::json::object response;
                            response["requestType"] = static_cast<int64_t>(WebRTCRequestState::RESTART);
                            response["state"] = 404;
                            response["message"] = "targetID is not register";
                            conn->webSocket->send(boost::json::serialize(response));

                            LOG_WARNING("目标用户未找到: %s (来自: %s)", targetID.c_str(), accountID.c_str());
                            return;
                        }

                        // 添加发送方信息
                        boost::json::object forwardMessage = val;
                        forwardMessage["state"] = 200;
                        forwardMessage["message"] = "WebRTCSignalServer forawrd";
                        targetConnection->webSocket->send(boost::json::serialize(forwardMessage));

                        LOG_INFO("消息转发成功: %s -> %s", accountID.c_str(), targetID.c_str());
                    }
                    else if (WebRTCRequestState(requestType) == WebRTCRequestState::STOPREMOTE) {
                        std::string accountID = val["accountID"].as_string().c_str();
                        std::string targetID = val["targetID"].as_string().c_str();

                        size_t hashSize = std::hash<std::string>{}(targetID) % this->hashValue;
                        std::shared_ptr<WebRTCConnection> targetConnection = nullptr;

                        {
                            std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[hashSize]);
                            auto it = webSocketPeerConnections[hashSize].find(targetID);
                            if (it != webSocketPeerConnections[hashSize].end()) {
                                targetConnection = it->second;
                            }
                        }

                        if (!targetConnection) {
                            boost::json::object response;
                            response["requestType"] = static_cast<int64_t>(WebRTCRequestState::STOPREMOTE);
                            response["state"] = 404;
                            response["message"] = "targetID is not register";
                            conn->webSocket->send(boost::json::serialize(response));

                            LOG_WARNING("目标用户未找到: %s (来自: %s)", targetID.c_str(), accountID.c_str());
                            return;
                        }

                        // 添加发送方信息
                        boost::json::object forwardMessage = val;
                        forwardMessage["state"] = 200;
                        forwardMessage["message"] = "WebRTCSignalServer forawrd";
                        targetConnection->webSocket->send(boost::json::serialize(forwardMessage));

                        LOG_INFO("消息转发成功: %s -> %s", accountID.c_str(), targetID.c_str());
                    }
                }
                catch (const std::exception& e) {
                    LOG_ERROR("消息处理错误: %s", e.what());
                }
            });

        // 连接断开处理
        ws->onClosed([this, conn = std::shared_ptr<WebRTCConnection>(connection)]() mutable {
            LOG_INFO("客户端断开连接: %s", conn->accountID.empty() ? conn->connectionID.c_str() : conn->accountID.c_str());
            removeConnection(conn);
            });

        ws->onError([this](const std::string& error) {
            LOG_ERROR("WebSocket连接错误: %s", error.c_str());
            });
        });

    LOG_INFO("WebRTC信令服务器启动成功 - 端口: %d, 哈希分片数: %zu", config.port, hashValue);
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

    // 停止WebSocket服务器
    if (webSocketServer) {
        try {
            webSocketServer->stop();
            LOG_INFO("WebSocket服务器已停止");
        }
        catch (const std::exception& e) {
            LOG_ERROR("停止WebSocket服务器时出错: %s", e.what());
        }
    }
}

void WebRTCSignalServer::clearAllConnections() {
    LOG_INFO("开始清理所有连接...");

    size_t totalConnections = 0;

    // 遍历所有哈希桶
    for (size_t i = 0; i < hashValue; ++i) {
        std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[i]);

        // 关闭所有WebSocket连接
        for (auto& pair : webSocketPeerConnections[i]) {
            if (pair.second && pair.second->webSocket) {
                try {
                    pair.second->webSocket->close();
                    totalConnections++;
                    LOG_DEBUG("关闭连接: %s", pair.first.c_str());
                }
                catch (const std::exception& e) {
                    LOG_ERROR("关闭连接 %s 时出错: %s", pair.first.c_str(), e.what());
                }
            }
        }

        // 清空连接映射
        webSocketPeerConnections[i].clear();
    }

    LOG_INFO("连接清理完成 - 共清理 %zu 个连接", totalConnections);
}

void WebRTCSignalServer::removeConnection(std::shared_ptr<WebRTCConnection> connection) {
    if (!connection || connection->accountID.empty()) {
        LOG_DEBUG("跳过移除无效连接");
        return;
    }

    try {
        std::lock_guard<std::mutex> hashLock(webSocketHashMutexs[connection->hashIndex]);
        auto it = webSocketPeerConnections[connection->hashIndex].find(connection->accountID);
        if (it != webSocketPeerConnections[connection->hashIndex].end()) {
            webSocketPeerConnections[connection->hashIndex].erase(it);
            LOG_INFO("成功移除连接: %s (哈希桶: %zu)", connection->accountID.c_str(), connection->hashIndex);
        }
        else {
            LOG_WARNING("尝试移除不存在的连接: %s", connection->accountID.c_str());
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("移除连接时出错: %s", e.what());
    }
}