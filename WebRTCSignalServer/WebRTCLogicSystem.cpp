#include "WebRTCLogicSystem.h"
#include "WebRTCSignalData.h"
#include "WebRTCSignalManager.h"

#include <chrono>

#include "Utils.h"


namespace Hope {

    WebRTCLogicSystem::WebRTCLogicSystem()
    {
        work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            boost::asio::make_work_guard(ioContext)
        );
  
    }

    void WebRTCLogicSystem::RunEventLoop() {

        initHandlers();

		auto self = shared_from_this();

        threads = std::move(std::thread([self]() {

            self->ioContext.run();

            }));
    }

    boost::asio::io_context& WebRTCLogicSystem::getIoCompletePorts()
    {
		return ioContext;
    }

    WebRTCLogicSystem::~WebRTCLogicSystem() {

        if (work) {
            work.reset();
        }

        ioContext.stop();

        if (threads.joinable()) {

            threads.join();

        }

    }

    void WebRTCLogicSystem::postMessageToQueue(std::shared_ptr<WebRTCSignalData> data) {

        boost::asio::post(ioContext, [this, data = std::move(data)]() {

			int type = data->json["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                webrtcHandlers[type](data);

            }
            else {
            
				LOG_ERROR("未知的 WebRTC 请求类型: %d", type);
                
            }

            });
      
    }


    void WebRTCLogicSystem::initHandlers() {
    
        auto self = shared_from_this();

        webrtcHandlers[static_cast<int>(WebRTCRequestState::REGISTER)] = [self](std::shared_ptr<WebRTCSignalData> data) {

			boost::json::object& message = data->json;

            auto webrtcSignalSocket = data->webrtcSignalSocket;

            if (!message.contains("accountID")) {
                LOG_WARNING("REGISTER 消息缺少 accountID.");
                return;
            }
            std::string accountID = message["accountID"].as_string().c_str();

            webrtcSignalSocket->setAccountID(accountID); // 假设 setAccountID 存在

            webrtcSignalSocket->setRegistered(true);

            data->webrtcSignalManager->webrtcSignalSocketMap[accountID] = webrtcSignalSocket;

            boost::json::object response;
            response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REGISTER);
            response["state"] = 200;
            response["message"] = "register successful";

            webrtcSignalSocket->writerAsync(boost::json::serialize(response));

            int mapChannelIndex = data->webrtcSignalManager->hasher(accountID) % data->webrtcSignalManager->hashSize;

            data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = data->webrtcSignalManager->shared_from_this(), accountID, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager) {

                manager->getActorSocketMappingIndex()[accountID] = self->channelIndex;

                });


            LOG_INFO("用户注册成功: %s (channelIndex: %d)", accountID.c_str(), data->webrtcSignalManager->channelIndex);

            };

        webrtcHandlers[static_cast<int>(WebRTCRequestState::REQUEST)] = [self](std::shared_ptr<WebRTCSignalData> data) {

            boost::json::object message = data->json;

            auto webrtcSignalSocket = data->webrtcSignalSocket;

            int64_t requestTypeValue = message["requestType"].as_int64();

            if (!message.contains("accountID") || !message.contains("targetID")) {
                LOG_WARNING("转发消息缺少 accountID 或 targetID.");
                return;
            }

            std::string accountID = message["accountID"].as_string().c_str(); // 发送方 ID
            std::string targetID = message["targetID"].as_string().c_str();   // 目标方 ID

            std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

            // 1. 查找目标连接 (使用哈希锁)
            {
                auto it = data->webrtcSignalManager->webrtcSignalSocketMap.find(targetID);
                if (it != data->webrtcSignalManager->webrtcSignalSocketMap.end()) {
                    targetSocket = it->second;
                }
            }

            // 2. 处理目标未找到 (404)
            if (!targetSocket) {

                tbb::concurrent_lru_cache<std::string, int>::handle handles = data->webrtcSignalManager->localRouteCache[targetID];

                auto self = data->webrtcSignalManager->shared_from_this();

                if (handles.value() == -1) {

                    int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                    data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                        if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                            int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                            self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                    if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                        handles.value() = manager->channelIndex;
                                    }

                                    boost::json::object forwardMessage = message; // 复制原始消息体
                                    forwardMessage["state"] = 200;
                                    forwardMessage["message"] = "WebRTCSignalServer forward";

                                    manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                    LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "REQUEST");

                                }
                                else {

                                    boost::json::object response;
                                    response["requestType"] = requestTypeValue;
                                    response["state"] = 404;
                                    response["message"] = "targetID is not register";
                                    webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                    LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "REQUEST");

                                }

                                });

                        }
                        else {

                            boost::json::object response;
                            response["requestType"] = requestTypeValue;
                            response["state"] = 404;
                            response["message"] = "targetID is not register";
                            webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                            LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "REQUEST");

                        }

                        });
                }
                else {

                    data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(handles.value(), [=](std::shared_ptr<WebRTCSignalManager> manager) {

                        if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                            if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                handles.value() = manager->channelIndex;
                            }

                            boost::json::object forwardMessage = message; // 复制原始消息体
                            forwardMessage["state"] = 200;
                            forwardMessage["message"] = "WebRTCSignalServer forward";

                            manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "REQUEST");

                        }
                        else {

                            int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                            data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                    int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                    self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                        if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                            if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                                handles.value() = manager->channelIndex;
                                            }

                                            boost::json::object forwardMessage = message; // 复制原始消息体
                                            forwardMessage["state"] = 200;
                                            forwardMessage["message"] = "WebRTCSignalServer forward";

                                            manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "REQUEST");

                                        }
                                        else {

                                            boost::json::object response;
                                            response["requestType"] = requestTypeValue;
                                            response["state"] = 404;
                                            response["message"] = "targetID is not register";
                                            webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                            LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "REQUEST");

                                        }

                                        });

                                }
                                else {

                                    boost::json::object response;
                                    response["requestType"] = requestTypeValue;
                                    response["state"] = 404;
                                    response["message"] = "targetID is not register";
                                    webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                    LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "REQUEST");

                                }

                                });

                        }

                        });

                }

                return;
            }

            // 3. 转发消息
            boost::json::object forwardMessage = message; // 复制原始消息体
            forwardMessage["state"] = 200;
            forwardMessage["message"] = "WebRTCSignalServer forward";
            targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "REQUEST");

            };

        webrtcHandlers[static_cast<int>(WebRTCRequestState::RESTART)] = [self](std::shared_ptr<WebRTCSignalData> data) {

            boost::json::object message = data->json;

            auto webrtcSignalSocket = data->webrtcSignalSocket;

            int64_t requestTypeValue = message["requestType"].as_int64();

            if (!message.contains("accountID") || !message.contains("targetID")) {
                LOG_WARNING("转发消息缺少 accountID 或 targetID.");
                return;
            }

            std::string accountID = message["accountID"].as_string().c_str(); // 发送方 ID
            std::string targetID = message["targetID"].as_string().c_str();   // 目标方 ID

            std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

            // 1. 查找目标连接 (使用哈希锁)
            {
                auto it = data->webrtcSignalManager->webrtcSignalSocketMap.find(targetID);
                if (it != data->webrtcSignalManager->webrtcSignalSocketMap.end()) {
                    targetSocket = it->second;
                }
            }

            // 2. 处理目标未找到 (404)
            if (!targetSocket) {

                tbb::concurrent_lru_cache<std::string, int>::handle handles = data->webrtcSignalManager->localRouteCache[targetID];

                auto self = data->webrtcSignalManager->shared_from_this();

                if (handles.value() == -1) {

                    int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                    data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                        if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                            int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                            self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                    if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                        handles.value() = manager->channelIndex;
                                    }

                                    boost::json::object forwardMessage = message; // 复制原始消息体
                                    forwardMessage["state"] = 200;
                                    forwardMessage["message"] = "WebRTCSignalServer forward";

                                    manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                    LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",accountID.c_str(), targetID.c_str(),"RESTART");

                                }
                                else {

                                    boost::json::object response;
                                    response["requestType"] = requestTypeValue;
                                    response["state"] = 404;
                                    response["message"] = "targetID is not register";
                                    webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                    LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",targetID.c_str(), accountID.c_str(), "RESTART");

                                }

                                });

                        }
                        else {

                            boost::json::object response;
                            response["requestType"] = requestTypeValue;
                            response["state"] = 404;
                            response["message"] = "targetID is not register";
                            webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                            LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",targetID.c_str(), accountID.c_str(),"RESTART");

                        }

                        });
                }
                else {

                    data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(handles.value(), [=](std::shared_ptr<WebRTCSignalManager> manager) {

                        if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                            if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                handles.value() = manager->channelIndex;
                            }

                            boost::json::object forwardMessage = message; // 复制原始消息体
                            forwardMessage["state"] = 200;
                            forwardMessage["message"] = "WebRTCSignalServer forward";

                            manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",accountID.c_str(), targetID.c_str(), "RESTART");

                        }
                        else {

                            int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                            data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                    int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                    self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                        if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                            if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                                handles.value() = manager->channelIndex;
                                            }

                                            boost::json::object forwardMessage = message; // 复制原始消息体
                                            forwardMessage["state"] = 200;
                                            forwardMessage["message"] = "WebRTCSignalServer forward";

                                            manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",accountID.c_str(), targetID.c_str(), "RESTART" );

                                        }
                                        else {

                                            boost::json::object response;
                                            response["requestType"] = requestTypeValue;
                                            response["state"] = 404;
                                            response["message"] = "targetID is not register";
                                            webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                            LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",targetID.c_str(), accountID.c_str(), "RESTART" );

                                        }

                                        });

                                }
                                else {

                                    boost::json::object response;
                                    response["requestType"] = requestTypeValue;
                                    response["state"] = 404;
                                    response["message"] = "targetID is not register";
                                    webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                    LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "RESTART");

                                }

                                });

                        }

                        });

                }

                return;
            }

            // 3. 转发消息
            boost::json::object forwardMessage = message; // 复制原始消息体
            forwardMessage["state"] = 200;
            forwardMessage["message"] = "WebRTCSignalServer forward";
            targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "RESTART");

            };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::STOPREMOTE)] = [self](std::shared_ptr<WebRTCSignalData> data) {

                boost::json::object message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountID") || !message.contains("targetID")) {
                    LOG_WARNING("转发消息缺少 accountID 或 targetID.");
                    return;
                }

                std::string accountID = message["accountID"].as_string().c_str(); // 发送方 ID
                std::string targetID = message["targetID"].as_string().c_str();   // 目标方 ID

                std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

                // 1. 查找目标连接 (使用哈希锁)
                {
                    auto it = data->webrtcSignalManager->webrtcSignalSocketMap.find(targetID);
                    if (it != data->webrtcSignalManager->webrtcSignalSocketMap.end()) {
                        targetSocket = it->second;
                    }
                }

                // 2. 处理目标未找到 (404)
                if (!targetSocket) {

                    tbb::concurrent_lru_cache<std::string, int>::handle handles = data->webrtcSignalManager->localRouteCache[targetID];

                    auto self = data->webrtcSignalManager->shared_from_this();

                    if (handles.value() == -1) {

                        int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                        data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                            if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                    if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                        if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                            handles.value() = manager->channelIndex;
                                        }

                                        boost::json::object forwardMessage = message; // 复制原始消息体
                                        forwardMessage["state"] = 200;
                                        forwardMessage["message"] = "WebRTCSignalServer forward";

                                        manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                        LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "STOPREMOTE");

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "STOPREMOTE");

                                    }

                                    });

                            }
                            else {

                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "targetID is not register";
                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "STOPREMOTE");

                            }

                            });
                    }
                    else {

                        data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(handles.value(), [=](std::shared_ptr<WebRTCSignalManager> manager) {

                            if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                    handles.value() = manager->channelIndex;
                                }

                                boost::json::object forwardMessage = message; // 复制原始消息体
                                forwardMessage["state"] = 200;
                                forwardMessage["message"] = "WebRTCSignalServer forward";

                                manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "STOPREMOTE");

                            }
                            else {

                                int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                                data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                    if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                        int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                        self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                                            if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                                    handles.value() = manager->channelIndex;
                                                }

                                                boost::json::object forwardMessage = message; // 复制原始消息体
                                                forwardMessage["state"] = 200;
                                                forwardMessage["message"] = "WebRTCSignalServer forward";

                                                manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "STOPREMOTE");

                                            }
                                            else {

                                                boost::json::object response;
                                                response["requestType"] = requestTypeValue;
                                                response["state"] = 404;
                                                response["message"] = "targetID is not register";
                                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                                LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "STOPREMOTE");

                                            }

                                            });

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), "STOPREMOTE");

                                    }

                                    });

                            }

                            });

                    }

                    return;
                }

                // 3. 转发消息
                boost::json::object forwardMessage = message; // 复制原始消息体
                forwardMessage["state"] = 200;
                forwardMessage["message"] = "WebRTCSignalServer forward";
                targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), "STOPREMOTE");

                };

                webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOSE)] = [self](std::shared_ptr<WebRTCSignalData> data) {

                    std::string accountID = data->webrtcSignalSocket->getAccountID();

                    if (!accountID.empty()) {

                        data->webrtcSignalManager->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
                    }
                    data->webrtcSignalSocket->stop(); // 关闭 socket 实例

                    LOG_INFO("收到用户 %s 的 CLOSE 请求，连接已停止", accountID.c_str());

                    };


    }

}



