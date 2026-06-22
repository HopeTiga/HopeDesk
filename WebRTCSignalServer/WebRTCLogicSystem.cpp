#include "WebRTCLogicSystem.h"
#include "WebRTCSignalServer.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "HttpSocket.h"
#include "WebRTCSignalPacket.h"
#include "WebRTCMysqlManagerPools.h"


#include <iostream>
#include <chrono>
#include <string_view>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include <boost/json.hpp>

#include <absl/strings/str_format.h>

#include "ConfigManager.h"

#include "AsyncTransactionGuard.h"

#include "Utils.h"

namespace hope {

    namespace core
    {

        WebRTCLogicSystem::WebRTCLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues)
            : ioContext(ioContext)
            , channelIndex(channelIndex)
            , taskQueues(taskQueues)
        {
            webrtcMysqlManagerPools = std::make_shared<hope::mysql::WebRTCMysqlManagerPools>(ioContext);

            threshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.threshold"));

            exitThreshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.exitThreshold"));

            asyncThreshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.asyncThreshold"));

        }

        boost::asio::io_context& WebRTCLogicSystem::getIoCompletePorts()
        {
            return ioContext;
        }

        WebRTCLogicSystem::~WebRTCLogicSystem() {

            closeEvent();

        }

        void WebRTCLogicSystem::asyncEvent() {

            if (asyncEvents.exchange(true)) return;

            initHandlers();

            initHttpHandlers();

        }

        void WebRTCLogicSystem::closeEvent() {

            if (!asyncEvents.exchange(false)) return;

            webrtcMysqlManagerPools.reset();

            webrtcHandlers.clear();

            httpHandlers.clear();

        }

        void WebRTCLogicSystem::asyncTaskExecute() {

            bool expected = false;

            if (!asyncTaskExecutes.compare_exchange_strong(expected, true)) return;

            boost::asio::co_spawn(ioContext, [this]()mutable->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::optional<AwaitableTask> optional = co_await taskQueues.dequeue();

                    if (!optional.has_value()) break;

                    AwaitableTask func = std::move(optional.value());

                    if (func) {

                        boost::asio::co_spawn(ioContext, [func = std::move(func), this]() mutable -> boost::asio::awaitable<void> {

                            co_await func();

                            }, [](std::exception_ptr ptr) {
                                if (ptr) {
                                    try {
                                        std::rethrow_exception(ptr);
                                    }
                                    catch (const std::exception& e) {
                                        LOG_ERROR("WebRTCLogicSystem asyncTaskExecute Task Exception: %s", e.what());
                                    }
                                }
                                });
                    }

                    if (localTaskQueueSize.load() >= exitThreshold.load()) {

                        LOG_WARN("WebRTCLogicSystem local queue depth %d exceeds threshold, switching to local processing", taskQueueSize.load());

                        asyncTaskExecutes.store(false);

                        break;
                    }

                    if (!asyncEvents.load()) {

                        asyncTaskExecutes.store(false);

                        break;

                    }

                }

                LOG_INFO("WebRTCLogicSystem asyncTaskExecute closeAsyncEvent");

                co_return;

                }, boost::asio::detached);

            return;

        }

        void WebRTCLogicSystem::postTaskAsync(std::shared_ptr<WebRTCSignalPacket> packet) {

            int type = packet->request["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalPacket>)>& func = webrtcHandlers[type];

                taskQueueSize.fetch_add(1);

                if (taskQueueSize.load() >= threshold.load() && localTaskQueueSize.load() >= threshold.load() && webrtcLogicHandlers[type]) {

                    std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = packet->webrtcSignalSocket->shared_from_this();

                    bool success = taskQueues.enqueue([this, type, &func, packet = std::move(packet)]()mutable -> boost::asio::awaitable<void> {

                        try {

                            co_await func(packet);

                        }
                        catch (...) {

                            taskQueueSize.fetch_sub(1);

                            throw;

                        }

                        taskQueueSize.fetch_sub(1);

                        co_return;

                        });

                    if (!success) {

                        taskQueueSize.fetch_sub(1);

                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%d,"state":503,"message":"webrtcSignalServer busy, please retry later"})", type));

                    }

                    return;

                }

                localTaskQueueSize.fetch_add(1);

                boost::asio::co_spawn(ioContext, [this, type, &func, packet]() mutable -> boost::asio::awaitable<void> {

                    co_await func(packet);

                    },
                    [this, type](std::exception_ptr ptr) {

                        taskQueueSize.fetch_sub(1);

                        if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

                            asyncTaskExecute();

                        }

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

        void WebRTCLogicSystem::postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest)
        {
            std::string targetUrl = httpRequest.target();

            if (httpHandlers.find(targetUrl) != httpHandlers.end()) {

                LOG_INFO("Http Request: %s", targetUrl.data());

                absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>& func = httpHandlers[targetUrl];

                taskQueueSize.fetch_add(1);

                if (taskQueueSize.load() >= threshold.load() && localTaskQueueSize.load() >= threshold.load() && httpLogicHandlers[targetUrl]) {

                    unsigned int version = httpRequest.version();

                    std::shared_ptr<HttpSocket> httpSocketShared = httpSocket->shared_from_this();

                    bool success = taskQueues.enqueue([this, httpSocket = std::move(httpSocket), httpRquest = std::move(httpRequest), &func]()mutable -> boost::asio::awaitable<void> {

                        try {

                            co_await func(httpSocket, httpRquest);

                        }
                        catch (...) {

                            taskQueueSize.fetch_sub(1);

                            throw;

                        }

                        taskQueueSize.fetch_sub(1);

                        co_return;

                        });

                    if (!success) {

                        taskQueueSize.fetch_sub(1);

                        boost::asio::io_context& ioContext = httpSocketShared->getIoContext();

                        boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocketShared), version]()mutable->boost::asio::awaitable<void> {

                            boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::ok, version };

                            httpResponse.set(boost::beast::http::field::content_type, "application/json");

                            httpResponse.body() = R"({"state":503,"message":"webrtcSignalServer busy, please retry later","data":null})";

                            httpResponse.prepare_payload();

                            httpResponse.keep_alive(httpSocket->getKeepAlive());

                            co_await httpSocket->asyncWrite(std::move(httpResponse));

                            co_return;

                            }, [this](std::exception_ptr ptr) {
                                if (ptr) {
                                    try {
                                        std::rethrow_exception(ptr);
                                    }
                                    catch (const std::exception& e) {
                                        LOG_ERROR("Overload boost::asio::co_spawn HttpTask Response Exception: %s", e.what());
                                    }
                                }
                                });

                    }

                    return;

                }

                localTaskQueueSize.fetch_add(1);

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRquest = std::move(httpRequest), &func]()mutable->boost::asio::awaitable<void> {

                    co_await func(httpSocket, httpRquest);

                    }, [this, targetUrl](std::exception_ptr ptr) {

                        taskQueueSize.fetch_sub(1);

                        if (localTaskQueueSize.fetch_sub(1) == 1) {

                            asyncTaskExecute();

                        }

                        if (ptr) {
                            try {

                                std::rethrow_exception(ptr);

                            }
                            catch (const std::exception& e) {

                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn HttpTask: %d Exception: %s", targetUrl.c_str(), e.what());

                            }
                        }
                        });

            }
            else {

                boost::asio::io_context& ioContext = httpSocket->getIoContext();

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRequest = std::move(httpRequest)]()mutable->boost::asio::awaitable<void> {

                    LOG_WARN("Http Request Not Found: %s", httpRequest.target().data());

                    boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::not_found,httpRequest.version() };

                    httpResponse.set(boost::beast::http::field::server, "WebRTCSignalServer");

                    httpResponse.set(boost::beast::http::field::content_type, "application/json");

                    httpResponse.keep_alive(httpSocket->getKeepAlive());

                    httpResponse.body() = R"({"state":404,"message":"The requested resource was not found on this server.","data":null})";

                    httpResponse.prepare_payload();

                    try {
                        co_await httpSocket->asyncWrite(std::move(httpResponse));

                        httpSocket->closeSocket();
                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("Http Write Error: %s", e.what());

                        httpSocket->closeSocket();

                    }

                    co_return;

                    }, [this, targetUrl](std::exception_ptr ptr) {
                        if (ptr) {
                            try {
                                std::rethrow_exception(ptr);
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn HttpTask: %s Exception: %s", targetUrl.c_str(), e.what());
                            }
                        }
                        });

            }

        }

        void WebRTCLogicSystem::initHandlers() {

            // Forward-200 response: mutates the moved-in request object (which carries
            // the variable forward payload, e.g. SDP) and serializes it. Used 6x across
            // the forward handler tree, so it is captured by forwardHandler and its
            // nested 200-lambdas rather than inlined.
            std::function<std::string(boost::json::object)> serializeForward200 = [](boost::json::object req) -> std::string {
                req["state"] = 200;
                req["message"] = "webrtcSignalServer forward";
                return boost::json::serialize(std::move(req));
                };

            // ==================== Forward Handler ====================
            std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalPacket>, std::string)> forwardHandler = [this, serializeForward200](std::shared_ptr<WebRTCSignalPacket> packet, std::string requestTypeStr)->boost::asio::awaitable<void> {

                boost::json::object& request = packet->request;

                auto webrtcSignalSocket = packet->webrtcSignalSocket;

                int64_t requestTypeValue = request["requestType"].as_int64();

                if (!request.contains("accountId") || !request.contains("targetId")) {

                    LOG_WARN("Forward Message Missing accountId or targetId.");

                    co_return;

                }

                std::string accountId = boost::json::value_to<std::string>(request["accountId"]);

                std::string targetId = boost::json::value_to<std::string>(request["targetId"]);

                std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

                {
                    auto it = packet->webrtcSignalManager->webrtcSocketMap.find(targetId);

                    if (it != packet->webrtcSignalManager->webrtcSocketMap.end()) {

                        targetSocket = it->second;

                    }

                }

                if (!targetSocket) {

                    auto it = packet->webrtcSignalSocket->actorMappingIndex.find(targetId);

                    int index = 0;

                    if (it == packet->webrtcSignalSocket->actorMappingIndex.end()) {

                        index = -1;

                    }
                    else {

                        index = it->second;

                    }

                    WebRTCSignalServer* webrtcSignalServer = packet->webrtcSignalManager->webrtcSignalServer;

                    int mapChannelIndex = packet->webrtcSignalManager->hasher(targetId.c_str()) % packet->webrtcSignalManager->hashSize;

                    int channelIndex = packet->webrtcSignalManager->getChannelIndex();

                    if (index == -1) {

                        packet->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), serializeForward200 = serializeForward200, request = std::move(packet->request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                if (targetChannelIndex == manager->getChannelIndex()) {

                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                        targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                        targetWebrtcSignalSocket->asyncWrite(serializeForward200(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }

                                    co_return;

                                }

                                manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), serializeForward200 = serializeForward200, request = std::move(request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                        targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                        targetWebrtcSignalSocket->asyncWrite(serializeForward200(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    });
                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                    co_return;

                                    });

                                co_return;

                            }
                            });
                    }
                    else {
                        packet->webrtcSignalManager->webrtcSignalServer->postTaskAsync(index, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), mapChannelIndex = std::move(mapChannelIndex), serializeForward200 = serializeForward200, request = std::move(packet->request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                targetWebrtcSignalSocket->asyncWrite(serializeForward200(std::move(request)));

                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                co_return;

                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), accountId, targetId, index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    auto routeIt = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                    if (routeIt != webrtcSignalSocket->actorMappingIndex.end() && routeIt->second == index) {

                                        webrtcSignalSocket->actorMappingIndex.erase(routeIt);

                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());
                                    }

                                    co_return;

                                    });


                                manager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), serializeForward200 = serializeForward200, request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                        if (targetChannelIndex == manager->getChannelIndex()) {

                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                                targetWebrtcSignalSocket->asyncWrite(serializeForward200(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }

                                            co_return;

                                        }

                                        manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), serializeForward200 = serializeForward200, request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                                targetWebrtcSignalSocket->asyncWrite(serializeForward200(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            });
                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }

                                    });
                            }

                            co_return;

                            });
                    }

                    co_return;

                }

                targetSocket->asyncWrite(serializeForward200(std::move(request)));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                };


            // ==================== Handler 0: REGISTER ====================
            webrtcHandlers[0] = [this](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> {

                boost::json::object& request = packet->request;

                if (!request.contains("accountId")) {

                    packet->webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":400,"message":"Missing accountId in registration request"})", static_cast<long long>(request["requestType"].as_int64())));

                    LOG_WARN("Registration Failed: Missing accountId");

                    co_return;

                }

                std::string accountId = boost::json::value_to<std::string>(request["accountId"]);

                packet->webrtcSignalSocket->setAccountId(accountId);

                packet->webrtcSignalSocket->setRegistered(true);

                packet->webrtcSignalManager->webrtcSocketMap[accountId] = packet->webrtcSignalSocket;

                packet->webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":200,"message":"Register Successful"})", static_cast<long long>(request["requestType"].as_int64())));

                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountId.c_str(), packet->webrtcSignalManager->channelIndex);

                int mapChannelIndex = packet->webrtcSignalManager->hasher(accountId) % packet->webrtcSignalManager->hashSize;

                packet->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [managers = packet->webrtcSignalManager->shared_from_this(), accountId, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->actorSocketMappingIndex[accountId] = managers->channelIndex;

                    co_return;

                    });

                co_return;

                };


            // ==================== Handlers 1-4 ====================
            webrtcHandlers[1] = [this, forwardHandler](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(packet), "REQUEST"); };
            webrtcHandlers[2] = [this, forwardHandler](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(packet), "RESTART"); };
            webrtcHandlers[3] = [this, forwardHandler](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(packet), "STOPREMOTE"); };

            webrtcHandlers[4] = [this](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> {

                std::string accountId = packet->webrtcSignalSocket->getAccountId();

                std::string sessionId = packet->webrtcSignalSocket->getSessionId();

                if (!accountId.empty()) {

                    packet->webrtcSignalManager->removeConnection(accountId, sessionId);

                }

                co_return;

                };

            webrtcHandlers[6] = [this, forwardHandler](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(packet), "CLOSESYSTEM");
                };

            webrtcHandlers[7] = [this, forwardHandler](std::shared_ptr<WebRTCSignalPacket> packet)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(packet), "SYSTEMREADLY");
                };

            webrtcLogicHandlers[0] = false;

            webrtcLogicHandlers[1] = false;

            webrtcLogicHandlers[2] = false;

            webrtcLogicHandlers[3] = false;

            webrtcLogicHandlers[4] = false;

            webrtcLogicHandlers[5] = false;

            webrtcLogicHandlers[6] = false;

            webrtcLogicHandlers[7] = false;

        }

        void WebRTCLogicSystem::initHttpHandlers()
        {

            std::function<void(std::shared_ptr<HttpSocket>, unsigned, std::string)> httpSocketAsyncWrite =
                [this](std::shared_ptr<HttpSocket> httpSocket, unsigned version, std::string body) {
                boost::asio::io_context& ioContext = httpSocket->getIoContext();
                boost::asio::co_spawn(
                    ioContext,
                    [httpSocket = std::move(httpSocket), version, body = std::move(body)]() mutable -> boost::asio::awaitable<void> {
                        boost::beast::http::response<boost::beast::http::string_body> res{
                            boost::beast::http::status::ok, version };
                        res.set(boost::beast::http::field::content_type, "application/json");
                        res.body() = std::move(body);
                        res.prepare_payload();
                        res.keep_alive(httpSocket->getKeepAlive());
                        co_await httpSocket->asyncWrite(std::move(res));
                        co_return;
                    },
                    [this](std::exception_ptr ptr) {
                        if (ptr) {
                            try {
                                std::rethrow_exception(ptr);
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn HttpTask Response Exception: %s", e.what());
                            }
                        }
                    });
                };


            // HTTP response carrying variable `data` (overview/stat) or a caller-
            // supplied `msg` (httpSocketAsyncWriteError — must go through boost::json
            // for safe string escaping). Fixed-message error responses are inlined
            // with absl::StrFormat at their call sites instead of coming through here.
            std::function<std::string(int, std::string_view, boost::json::value)> serializeHttpResp =
                [](int state, std::string_view message, boost::json::value data) -> std::string {
                boost::json::storage_ptr sp = boost::json::make_shared_resource<boost::json::monotonic_resource>();
                boost::json::object resp(sp);
                resp["state"] = state;
                resp["message"] = message;
                resp["data"] = std::move(data);
                return boost::json::serialize(resp);
                };

            std::function<void(std::shared_ptr<HttpSocket>, unsigned, int, std::string)> httpSocketAsyncWriteError =
                [this, httpSocketAsyncWrite, serializeHttpResp](std::shared_ptr<HttpSocket> httpSocket, unsigned version, int code, std::string msg) {
                httpSocketAsyncWrite(httpSocket, version, serializeHttpResp(code, msg, nullptr));
                };


            std::function<bool(const boost::beast::http::request<boost::beast::http::string_body>&)> verifyAuthorization =
                [this, httpSocketAsyncWrite, httpSocketAsyncWriteError](const boost::beast::http::request<boost::beast::http::string_body>& req) {
                try {
                    auto it = req.find(boost::beast::http::field::authorization);
                    if (it == req.end()) return false;
                    std::string_view authView{ it->value().data(), it->value().size() };
                    if (authView.size() < 7 || authView.substr(0, 7) != "Bearer ")
                        return false;
                    return authView.substr(7) == "913140924@qq.com";
                }
                catch (std::exception& e) {
                    LOG_ERROR("Authorization Verify Error:%s", e.what());
                    return false;
                }
                };

            // -------- 路由 /api/v1/managers/overview --------
            httpHandlers["/api/v1/managers/overview"] =
                [this, httpSocketAsyncWrite, httpSocketAsyncWriteError, verifyAuthorization, serializeHttpResp](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {
                        if (!verifyAuthorization(httpRequest)) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 403, "Forbidden");
                            co_return;
                        }

                        WebRTCSignalServer* server = httpSocket->getWebRTCSignalManager()->webrtcSignalServer;

                        boost::json::object data;
                        data["totalManagers"] = server->getChannelNumbers();

                        httpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(200, "success", std::move(data)));
                        co_return;
                };

            // -------- 路由 /api/v1/managers/stat --------
            httpHandlers["/api/v1/managers/stat"] =
                [this, httpSocketAsyncWrite, httpSocketAsyncWriteError, verifyAuthorization](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {
                        if (!verifyAuthorization(httpRequest)) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 403, "Forbidden");
                            co_return;
                        }

                        unsigned char parseBuf[256];
                        boost::json::monotonic_resource parseMr(parseBuf, sizeof(parseBuf));
                        boost::json::value reqBody;
                        try {
                            reqBody = boost::json::parse(httpRequest.body(), &parseMr);
                        }
                        catch (const boost::system::system_error&) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Invalid JSON body");
                            co_return;
                        }

                        if (!reqBody.is_object()) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Request body must be an object");
                            co_return;
                        }
                        auto& obj = reqBody.as_object();
                        auto it = obj.find("channelIndex");
                        if (it == obj.end() || !it->value().is_int64()) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Missing or invalid channelIndex");
                            co_return;
                        }
                        size_t targetIdx = static_cast<size_t>(it->value().as_int64());

                        auto manager = httpSocket->getWebRTCSignalManager();
                        WebRTCSignalServer* server = manager->webrtcSignalServer;
                        if (targetIdx >= server->getChannelNumbers()) {
                            httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Invalid channelIndex");
                            co_return;
                        }

                        int currentChannelIndex = manager->channelIndex;

                        if (targetIdx == static_cast<size_t>(currentChannelIndex)) {
                            boost::json::storage_ptr sp = boost::json::make_shared_resource<boost::json::monotonic_resource>();
                            boost::json::array socketList(sp);
                            socketList.reserve(manager->webrtcSocketMap.size());
                            for (auto const& [accountId, socketPtr] : manager->webrtcSocketMap) {
                                boost::json::object sInfo(sp);
                                sInfo["accountId"] = accountId;
                                sInfo["remoteAddr"] = socketPtr->getRemoteAddress();
                                sInfo["sessionId"] = socketPtr->getSessionId();
                                sInfo["isRegistered"] = socketPtr->getRegistered();
                                sInfo["cachedRouteCount"] = static_cast<std::int64_t>(socketPtr->actorMappingIndex.size());
                                socketList.emplace_back(std::move(sInfo));
                            }

                            boost::json::object targetData(sp);
                            targetData["channelIndex"] = currentChannelIndex;
                            targetData["totalSockets"] = static_cast<std::int64_t>(manager->webrtcSocketMap.size());
                            targetData["sockets"] = std::move(socketList);

                            boost::json::object resp(sp);
                            resp["state"] = 200;
                            resp["message"] = "success";
                            resp["data"] = std::move(targetData);

                            httpSocketAsyncWrite(httpSocket, httpRequest.version(), boost::json::serialize(resp));
                            co_return;
                        }

                        server->postTaskAsync(
                            targetIdx,
                            [this, httpSocket = httpSocket->shared_from_this(), version = httpRequest.version(),
                            currentChannelIndex, httpSocketAsyncWrite](
                                std::shared_ptr<WebRTCSignalManager> targetManager) mutable -> boost::asio::awaitable<void> {
                                    boost::json::storage_ptr sp = boost::json::make_shared_resource<boost::json::monotonic_resource>();
                                    boost::json::array socketList(sp);
                                    socketList.reserve(targetManager->webrtcSocketMap.size());
                                    for (auto const& [accountId, socketPtr] : targetManager->webrtcSocketMap) {
                                        boost::json::object sInfo(sp);
                                        sInfo["accountId"] = accountId;
                                        sInfo["remoteAddr"] = socketPtr->getRemoteAddress();
                                        sInfo["sessionId"] = socketPtr->getSessionId();
                                        sInfo["isRegistered"] = socketPtr->getRegistered();
                                        sInfo["cachedRouteCount"] = static_cast<std::int64_t>(socketPtr->actorMappingIndex.size());
                                        socketList.emplace_back(std::move(sInfo));
                                    }

                                    boost::json::object targetData(sp);
                                    targetData["channelIndex"] = targetManager->channelIndex;
                                    targetData["totalSockets"] = static_cast<std::int64_t>(targetManager->webrtcSocketMap.size());
                                    targetData["sockets"] = std::move(socketList);

                                    targetManager->webrtcSignalServer->postTaskAsync(
                                        currentChannelIndex,
                                        [this, httpSocket, version, targetData = std::move(targetData), sp,
                                        httpSocketAsyncWrite](std::shared_ptr<WebRTCSignalManager> manager) mutable -> boost::asio::awaitable<void> {
                                            boost::json::object resp(sp);
                                            resp["state"] = 200;
                                            resp["message"] = "success";
                                            resp["data"] = std::move(targetData);
                                            httpSocketAsyncWrite(httpSocket, version, boost::json::serialize(resp));
                                            co_return;
                                        });
                                    co_return;
                            });
                        co_return;
                };

            httpLogicHandlers["/api/v1/managers/overview"] = true;
            httpLogicHandlers["/api/v1/managers/stat"] = true;
        }

    }

}