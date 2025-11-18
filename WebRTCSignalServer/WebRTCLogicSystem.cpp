#include "WebRTCLogicSystem.h"
#include "WebRTCSignalData.h"
#include "WebRTCSignalManager.h"
#include "WebRTCMysqlManagerPools.h"

#include <iostream>
#include <chrono>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include "AsyncTransactionGuard.h"

#include "ConfigManager.h"
#include "Utils.h"

constexpr static char secretKey[] = "913140924@qq.com";

namespace hope {

    namespace core
    {

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

        void WebRTCLogicSystem::postTaskAsync(std::shared_ptr<WebRTCSignalData> data) {

            int type = data->json["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>> pairs = this->webrtcHandlers[type];

                if (pairs.first) {

                    std::shared_ptr<WebRTCMysqlManager> manager = WebRTCMysqlManagerPools::getInstance()->getTransactionMysqlManager();

                    if (manager) {

                        boost::asio::co_spawn(ioContext, [this, type, pairs, manager, data]() mutable -> boost::asio::awaitable<void> {

                            try {
                                co_await pairs.second(data, manager);
                            }
                            catch (...) {
                                WebRTCMysqlManagerPools::getInstance()->returnTransactionMysqlManager(std::move(manager));
                                throw;
                            }

                            WebRTCMysqlManagerPools::getInstance()->returnTransactionMysqlManager(std::move(manager));
                            },
                            [this, type](std::exception_ptr ptr) {
                                if (ptr) {
                                    try {
                                        std::rethrow_exception(ptr);
                                    }
                                    catch (const std::exception& e) {
                                        LOG_ERROR("webrtcLogicSystem boost::asio::co_spawn Task: %d Exception: %s", type, e.what());
                                    }
                                }
                            });

                    }
                    else {
                        postTaskAsync(data); // 暂不加重试，保持原样
                    }

                }
                else {
                    std::shared_ptr<WebRTCMysqlManager> manager = WebRTCMysqlManagerPools::getInstance()->getMysqlManager();

                    boost::asio::co_spawn(ioContext, [this, type, pairs, manager, data]() -> boost::asio::awaitable<void> {
                        co_await pairs.second(data, manager);
                        },
                        [this, type](std::exception_ptr ptr) {
                            if (ptr) {
                                try {
                                    std::rethrow_exception(ptr);
                                }
                                catch (const std::exception& e) {
                                    LOG_ERROR("webrtcLogicSystem boost::asio::co_spawn Task: %d Exception: %s", type, e.what());
                                }
                            }
                        });
                }

            }
            else {
                LOG_ERROR("Unknown WebRTC Request Type: %d", type);
            }
        }


        void WebRTCLogicSystem::initHandlers() {

            auto self = shared_from_this();

            std::function<boost::asio::awaitable <void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>, std::string)> forwardHandler = [self](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager>, std::string requestTypeStr)->boost::asio::awaitable<void> {

                boost::json::object message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountID") || !message.contains("targetID")) {
                    LOG_WARNING("Forward Message Missing accountID or targetID.");
                    co_return;
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

                        data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) ->boost::asio::awaitable<void> {

                            if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                                    if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                        if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                            handles.value() = manager->channelIndex;
                                        }

                                        boost::json::object forwardMessage = message; // 复制原始消息体
                                        forwardMessage["state"] = 200;
                                        forwardMessage["message"] = "WebRTCSignalServer forward";

                                        manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                        co_return;

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                        co_return;

                                    }

                                    });

                            }
                            else {

                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "targetID is not register";
                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                co_return;

                            }

                            });
                    }
                    else {

                        data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(handles.value(), [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                            if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                    handles.value() = manager->channelIndex;
                                }

                                boost::json::object forwardMessage = message; // 复制原始消息体
                                forwardMessage["state"] = 200;
                                forwardMessage["message"] = "WebRTCSignalServer forward";

                                manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                co_return;

                            }
                            else {

                                int mapChannelIndex = data->webrtcSignalManager->hasher(targetID) % data->webrtcSignalManager->hashSize;

                                data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                                    if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {

                                        int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                                        self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) ->boost::asio::awaitable<void> {

                                            if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {

                                                if (tbb::concurrent_lru_cache<std::string, int>::handle handles = self->localRouteCache[targetID]) {
                                                    handles.value() = manager->channelIndex;
                                                }

                                                boost::json::object forwardMessage = message; // 复制原始消息体
                                                forwardMessage["state"] = 200;
                                                forwardMessage["message"] = "WebRTCSignalServer forward";

                                                manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                                co_return;

                                            }
                                            else {

                                                boost::json::object response;
                                                response["requestType"] = requestTypeValue;
                                                response["state"] = 404;
                                                response["message"] = "targetID is not register";
                                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                                LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                                co_return;

                                            }

                                            });

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                        co_return;
                                    }

                                    });

                            }

                            });

                    }

                    co_return;
                }

                // 3. 转发消息
                boost::json::object forwardMessage = message; // 复制原始消息体
                forwardMessage["state"] = 200;
                forwardMessage["message"] = "WebRTCSignalServer forward";
                targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REGISTER)] = std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>>(false, [self](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager > mysqlManager)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                boost::json::object response;

                response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REGISTER);

                if (!message.contains("accountID")) {

                    LOG_WARNING("REGISTER Message Missing AccountID.");

                    response["state"] = 500;

                    response["message"] = "REGISTER Message Missing AccountID.";

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    co_return;
                }

                std::string accountID = message["accountID"].as_string().c_str();

                webrtcSignalSocket->setAccountID(accountID); // 假设 setAccountID 存在

                webrtcSignalSocket->setRegistered(true);

                data->webrtcSignalManager->webrtcSignalSocketMap[accountID] = webrtcSignalSocket;

                response["state"] = 200;

                response["message"] = "register successful";

                webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                int mapChannelIndex = data->webrtcSignalManager->hasher(accountID) % data->webrtcSignalManager->hashSize;

                data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = data->webrtcSignalManager->shared_from_this(), accountID, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->getActorSocketMappingIndex()[accountID] = self->channelIndex;

                    co_return;

                    });


                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountID.c_str(), data->webrtcSignalManager->channelIndex);

                });

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REQUEST)] = std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>>(false, [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager> mysqlManager)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), mysqlManager, "REQUEST");

                });

            webrtcHandlers[static_cast<int>(WebRTCRequestState::RESTART)] = std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>>(false, [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager> mysqlManager)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), mysqlManager, "RESTART");

                });

            webrtcHandlers[static_cast<int>(WebRTCRequestState::STOPREMOTE)] = std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>>(false, [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager> mysqlManager)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), mysqlManager, "STOPREMOTE");

                });

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOSE)] = std::pair<bool, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::shared_ptr<WebRTCMysqlManager>)>>(false, [self](std::shared_ptr<WebRTCSignalData> data, std::shared_ptr<WebRTCMysqlManager> mysqlManger)->boost::asio::awaitable<void> {

                std::string accountID = data->webrtcSignalSocket->getAccountID();

                if (!accountID.empty()) {

                    data->webrtcSignalManager->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
                }

                data->webrtcSignalSocket->stop(); // 关闭 socket 实例

                LOG_INFO("Receive User %s CLOSE Request，WebRTCSignalSocket is Stop", accountID.c_str());

                co_return;
                });

        }


    }

}



