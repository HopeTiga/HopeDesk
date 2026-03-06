#include "WebRTCLogicSystem.h"
#include "WebRTCSignalServer.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "WebRTCSignalData.h"
#include "WebRTCMysqlManagerPools.h"


#include <iostream>
#include <chrono>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include "WebRTCHashMap.h"
#include "WebRTCHashSet.h"

#include "AsyncTransactionGuard.h"

#include "Utils.h"


namespace hope {

    namespace core
    {

        WebRTCLogicSystem::WebRTCLogicSystem(boost::asio::io_context& ioContext) : ioContext(ioContext)
        {


        }

        void WebRTCLogicSystem::RunEventLoop() {

            initHandlers();

        }

        boost::asio::io_context& WebRTCLogicSystem::getIoCompletePorts()
        {
            return ioContext;
        }

        WebRTCLogicSystem::~WebRTCLogicSystem() {
            webrtcHandlers.clear();
        }

        void WebRTCLogicSystem::postTaskAsync(std::shared_ptr<WebRTCSignalData> data) {

            data->json = makeCleanCopy(data->json);

            int type = data->json["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>)> func = this->webrtcHandlers[type];

                boost::asio::co_spawn(ioContext, [this, type, func, data]() mutable -> boost::asio::awaitable<void> {
                    try {
                        co_await func(data);
                    }
                    catch (...) {
                        throw;
                    }

                    },
                    [this, type](std::exception_ptr ptr) {
                        if (ptr) {
                            try {
                                std::rethrow_exception(ptr);
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn Task: %d Exception: %s", type, e.what());
                            }
                        }
                    });

            }
            else {
                LOG_ERROR("Unknown WebRTC Request Type: %d", type);
            }
        }

        void WebRTCLogicSystem::initHandlers() {

            auto self = shared_from_this();

            // ==================== Forward Handler ====================
            std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::string)> forwardHandler = [self](std::shared_ptr<WebRTCSignalData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {
                boost::json::object message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;
                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountId") || !message.contains("targetId")) {
                    LOG_WARNING("Forward Message Missing accountId or targetId.");
                    co_return;
                }

                std::string accountId = message["accountId"].as_string().c_str();
                std::string targetId = message["targetId"].as_string().c_str();
                std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

                // 1. 查找目标连接 (使用哈希锁)
                {
                    auto it = data->webrtcSignalManager->webrtcSocketMap.find(targetId);
                    if (it != data->webrtcSignalManager->webrtcSocketMap.end()) {
                        targetSocket = it->second;
                    }
                }

                // 2. 处理目标未找到 (404)
                if (!targetSocket) {
                    tbb::concurrent_lru_cache<std::string, int>::handle handles = data->webrtcSignalManager->localRouteCache[targetId];
                    auto self = data->webrtcSignalManager->shared_from_this();

                    if (handles.value() == -1) {
                        int mapChannelIndex = data->webrtcSignalManager->hasher(targetId) % data->webrtcSignalManager->hashSize;

                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {
                            if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {
                                int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                self->webrtcSignalServer->postTaskAsync(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {
                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {
                                        if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetId]) {
                                            handles.value() = manager->channelIndex;
                                        }
                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];
                                        boost::json::object forwardMessage = message;
                                        forwardMessage["state"] = 200;
                                        forwardMessage["message"] = "MsquicServer forward";

                                        co_await targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                        co_return;
                                    }
                                    else {
                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "TargetId is not register";

                                        co_await webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                        LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                        co_return;
                                    }
                                    });
                            }
                            else {
                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "TargetId is not register";

                                co_await webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                co_return;
                            }
                            });
                    }
                    else {
                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(handles.value(), [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {
                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {
                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetId]) {
                                    handles.value() = manager->channelIndex;
                                }
                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];
                                boost::json::object forwardMessage = message;
                                forwardMessage["state"] = 200;
                                forwardMessage["message"] = "MsquicServer forward";

                                co_await targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                co_return;
                            }
                            else {

                                tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetId];

                                handles.value() = -1;

                                int mapChannelIndex = data->webrtcSignalManager->hasher(targetId) % data->webrtcSignalManager->hashSize;

                                data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {
                                    if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {
                                        int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                        self->webrtcSignalServer->postTaskAsync(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {
                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {
                                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetId]) {
                                                    handles.value() = manager->channelIndex;
                                                }
                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];
                                                boost::json::object forwardMessage = message;
                                                forwardMessage["state"] = 200;
                                                forwardMessage["message"] = "MsquicServer forward";

             
                                                co_await targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                                co_return;
                                            }
                                            else {
                                                boost::json::object response;
                                                response["requestType"] = requestTypeValue;
                                                response["state"] = 404;
                                                response["message"] = "TargetId is not register";

           
                                                co_await webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                                LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                                co_return;
                                            }
                                            });
                                    }
                                    else {
                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "TargetId is not register";

                                        co_await webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                        LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                                        co_return;
                                    }
                                    });
                            }
                            });
                    }
                    co_return;
                }

                // 3. 转发消息
                boost::json::object forwardMessage = message;

                forwardMessage["state"] = 200;

                forwardMessage["message"] = "webrtcSignalServer forward";

                co_await targetSocket->asyncWrite(boost::json::serialize(forwardMessage));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr);
                };


            // ==================== Handler 0: REGISTER ====================
            webrtcHandlers[0] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                boost::json::object response;

                response["requestType"] = message["requestType"].as_int64();

                if (!message.contains("accountId")) {
                
                    response["state"] = 400;
                    response["message"] = "Missing accountId in registration request";
                    co_await data->webrtcSignalSocket->asyncWrite(boost::json::serialize(response));
					data->webrtcSignalSocket->closeSocket();
                    LOG_WARNING("Registration Failed: Missing accountId");
					co_return;

                }

				std::string accountId = message["accountId"].as_string().c_str();


                data->webrtcSignalSocket->setAccountId(accountId);

                data->webrtcSignalSocket->setRegistered(true);

                data->webrtcSignalManager->webrtcSocketMap[accountId] = data->webrtcSignalSocket;

                response["state"] = 200;

                response["message"] = "Register Successful";

                response["accountId"] = accountId;

                co_await data->webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountId.c_str(), data->webrtcSignalManager->channelIndex);

                int mapChannelIndex = data->webrtcSignalManager->hasher(accountId) % data->webrtcSignalManager->hashSize;

                data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [ managers = data->webrtcSignalManager->shared_from_this(), accountId, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->actorSocketMappingIndex[accountId] = managers->channelIndex;

                    co_return;

                    });

                co_return;

                };


            // ==================== Handlers 1-4 ====================
            webrtcHandlers[1] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "REQUEST"); };
            webrtcHandlers[2] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "RESTART"); };
            webrtcHandlers[3] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "STOPREMOTE"); };

            webrtcHandlers[4] = [self](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {

                std::string accountId = data->webrtcSignalSocket->getAccountId();

                std::string sessionId = data->webrtcSignalSocket->getSessionId();

                if (!accountId.empty()) {

                    data->webrtcSignalManager->removeConnection(accountId, sessionId);

                }

                co_return;

                };

            webrtcHandlers[6] = [self, forwardHandler](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "CLOSESYSTEM");
                };

            webrtcHandlers[7] = [self, forwardHandler](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "SYSTEMREADLY");
                };

        }

    }

}