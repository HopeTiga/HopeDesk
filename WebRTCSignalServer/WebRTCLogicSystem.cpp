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

        thread_local int threadChannelIndex = -1;

        WebRTCLogicSystem::WebRTCLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues)
            : ioContext(ioContext)
            , channelIndex(channelIndex)
            , taskQueues(taskQueues)
        {
            webrtcMysqlManagerPools = std::make_shared<hope::mysql::WebRTCMysqlManagerPools>(ioContext);

            threshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.threshold"));

            exitThreshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.exitThreshold"));

            asyncThreshold.store(ConfigManager::Instance().GetInt("WebRTCSignalServer.asyncThreshold"));

            boost::asio::post(ioContext, [channelIndex]() {
                
                threadChannelIndex = channelIndex;

                });

        }

        boost::asio::io_context& WebRTCLogicSystem::getIoCompletionPorts()
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

        void WebRTCLogicSystem::postTaskAsync(WebRTCSignalPacket webrtcSignalPacket) {

            int type = webrtcSignalPacket.request["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                absl::AnyInvocable<boost::asio::awaitable<void>(WebRTCSignalPacket)>& func = webrtcHandlers[type];

                taskQueueSize.fetch_add(1);

                if (taskQueueSize.load() >= threshold.load() && localTaskQueueSize.load() >= threshold.load() && webrtcLogicHandlers[type]) {

                    std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket->shared_from_this();

                    bool success = taskQueues.enqueue([this, type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]()mutable -> boost::asio::awaitable<void> {

                        try {

                            co_await func(std::move(webrtcSignalPacket));

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

                boost::asio::co_spawn(ioContext, [this, type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<void> {

                    co_await func(std::move(webrtcSignalPacket));

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

                        if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

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

            // ==================== Forward Handler ====================
            std::function<boost::asio::awaitable<void>(WebRTCSignalPacket, std::string)> forwardHandler = [this](WebRTCSignalPacket webrtcSignalPacket, std::string requestTypeStr)->boost::asio::awaitable<void> {

                boost::json::object& request = webrtcSignalPacket.request;

                auto webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket;

                int64_t requestTypeValue = request["requestType"].as_int64();

                if (!request.contains("accountId") || !request.contains("targetId")) {

                    LOG_WARN("Forward Message Missing accountId or targetId.");

                    co_return;

                }

                std::string accountId = boost::json::value_to<std::string>(request["accountId"]);

                std::string targetId = boost::json::value_to<std::string>(request["targetId"]);

                std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

                {
                    auto it = webrtcSignalPacket.webrtcSignalManager->webrtcSocketMap.find(targetId);

                    if (it != webrtcSignalPacket.webrtcSignalManager->webrtcSocketMap.end()) {

                        targetSocket = it->second;

                    }

                }

                if (!targetSocket) {

                    auto it = webrtcSignalPacket.webrtcSignalSocket->actorMappingIndex.find(targetId);

                    int index = 0;

                    if (it == webrtcSignalPacket.webrtcSignalSocket->actorMappingIndex.end()) {

                        index = -1;

                    }
                    else {

                        index = it->second;

                    }

                    WebRTCSignalServer* webrtcSignalServer = webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer;

                    int mapChannelIndex = webrtcSignalPacket.webrtcSignalManager->hasher(targetId.c_str()) % webrtcSignalPacket.webrtcSignalManager->hashSize;

                    int channelIndex = webrtcSignalPacket.webrtcSignalManager->getChannelIndex();

                    if (index == -1) {

                        webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(webrtcSignalPacket.request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            auto indexIt = manager->actorSocketMappingIndex.find(targetId);

                            if (indexIt != manager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = indexIt->second;

                                if (targetChannelIndex == manager->getChannelIndex()) {

                                    auto it = manager->webrtcSocketMap.find(targetId);

                                    if ( it != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = it->second;

                                        request["state"] = 200;

                                        request["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex , targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }

                                    co_return;

                                }

                                manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    auto it = manager->webrtcSocketMap.find(targetId);

                                    if ( it != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = it->second;

                                        request["state"] = 200;

                                        request["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        int targetChannelIndex = manager->channelIndex;

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    });
                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                    co_return;

                                    });

                                co_return;

                            }
                            });
                    }
                    else {

                        webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTaskAsync(index, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), mapChannelIndex = std::move(mapChannelIndex), request = std::move(webrtcSignalPacket.request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            auto it = manager->webrtcSocketMap.find(targetId);

                            if (it != manager->webrtcSocketMap.end()) {

                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = it->second;

                                request["state"] = 200;

                                request["message"] = "webrtcSignalServer forward";

                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                co_return;

                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    auto indexIt = manager->actorSocketMappingIndex.find(targetId);

                                    if (indexIt != manager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = indexIt->second;

                                        if (targetChannelIndex == manager->getChannelIndex()) {

                                            auto it = manager->webrtcSocketMap.find(targetId);

                                            if (it != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = it->second;

                                                request["state"] = 200;

                                                request["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

                                                    webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId),index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    auto routeIt = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                                    if (routeIt != webrtcSignalSocket->actorMappingIndex.end() && routeIt->second == index) {

                                                        webrtcSignalSocket->actorMappingIndex.erase(routeIt);

                                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                                    }

                                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }

                                            co_return;

                                        }

                                        manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId),index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            auto it = manager->webrtcSocketMap.find(targetId);

                                            if (it != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = it->second;

                                                request["state"] = 200;

                                                request["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                int targetChannelIndex = manager->channelIndex;

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

                                                    webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId),index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    auto routeIt = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                                    if (routeIt != webrtcSignalSocket->actorMappingIndex.end() && routeIt->second == index) {

                                                        webrtcSignalSocket->actorMappingIndex.erase(routeIt);

                                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                                    }

                                                    webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            });
                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId),index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            auto routeIt = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                            if (routeIt != webrtcSignalSocket->actorMappingIndex.end() && routeIt->second == index) {

                                                webrtcSignalSocket->actorMappingIndex.erase(routeIt);

                                                LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                            }

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

                request["state"] = 200;

                request["message"] = "webrtcSignalServer forward";

                targetSocket->asyncWrite(boost::json::serialize(std::move(request)));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                };

            // ==================== Handler 0: REGISTER ====================
            webrtcHandlers[0] = [this](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {

                boost::json::object& request = webrtcSignalPacket.request;

                if (!request.contains("accountId")) {

                    webrtcSignalPacket.webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":400,"message":"Missing accountId in registration request"})", static_cast<long long>(request["requestType"].as_int64())));

                    LOG_WARN("Registration Failed: Missing accountId");

                    co_return;

                }

                std::string accountId = boost::json::value_to<std::string>(request["accountId"]);

                webrtcSignalPacket.webrtcSignalSocket->setAccountId(accountId);

                webrtcSignalPacket.webrtcSignalSocket->setRegistered(true);

                webrtcSignalPacket.webrtcSignalManager->webrtcSocketMap[accountId] = webrtcSignalPacket.webrtcSignalSocket;

                webrtcSignalPacket.webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":200,"message":"Register Successful"})", static_cast<long long>(request["requestType"].as_int64())));

                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountId.c_str(), webrtcSignalPacket.webrtcSignalManager->channelIndex);

                int mapChannelIndex = webrtcSignalPacket.webrtcSignalManager->hasher(accountId) % webrtcSignalPacket.webrtcSignalManager->hashSize;

                webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [managers = webrtcSignalPacket.webrtcSignalManager->shared_from_this(), accountId, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->actorSocketMappingIndex[accountId] = managers->channelIndex;

                    co_return;

                    });

                co_return;

                };


            // ==================== Handlers 1-4 ====================
            webrtcHandlers[1] = [this, forwardHandler](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(webrtcSignalPacket), "REQUEST"); };
            webrtcHandlers[2] = [this, forwardHandler](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(webrtcSignalPacket), "RESTART"); };
            webrtcHandlers[3] = [this, forwardHandler](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(webrtcSignalPacket), "STOPREMOTE"); };

            webrtcHandlers[4] = [this](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {

                std::string accountId = webrtcSignalPacket.webrtcSignalSocket->getAccountId();

                std::string sessionId = webrtcSignalPacket.webrtcSignalSocket->getSessionId();

                if (!accountId.empty()) {

                    webrtcSignalPacket.webrtcSignalManager->removeConnection(accountId, sessionId);

                }

                co_return;

                };

            webrtcHandlers[6] = [this, forwardHandler](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(webrtcSignalPacket), "CLOSESYSTEM");
                };

            webrtcHandlers[7] = [this, forwardHandler](WebRTCSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(webrtcSignalPacket), "SYSTEMREADLY");
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

            std::function<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, unsigned, std::string)> awaitableHttpSocketAsyncWrite =
                [this](std::shared_ptr<HttpSocket> httpSocket, unsigned version, std::string body) mutable->boost::asio::awaitable<void> {

                boost::beast::http::response<boost::beast::http::string_body> res{
                           boost::beast::http::status::ok, version };

                res.set(boost::beast::http::field::content_type, "application/json");

                res.body() = std::move(body);

                res.prepare_payload();

                res.keep_alive(httpSocket->getKeepAlive());

                co_await httpSocket->asyncWrite(std::move(res));

                co_return;

                };

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


            std::function<bool(const boost::beast::http::request<boost::beast::http::string_body>&)> verifyAuthorization =
                [this, httpSocketAsyncWrite](const boost::beast::http::request<boost::beast::http::string_body>& req) {
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
                [this, httpSocketAsyncWrite, verifyAuthorization, serializeHttpResp, awaitableHttpSocketAsyncWrite](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {
                        if (!verifyAuthorization(httpRequest)) {

                            co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(403, "Forbidden",nullptr));
   
                            co_return;
                        }

                        WebRTCSignalServer* server = httpSocket->getWebRTCSignalManager()->webrtcSignalServer;

                        boost::json::object data;

                        data["totalManagers"] = server->getChannelNumbers();

                        LOG_INFO("channelIndex:%d threadChannelIndex:%d",httpSocket->getWebRTCSignalManager()->getChannelIndex() , threadChannelIndex);

                        if (httpSocket->getWebRTCSignalManager()->getChannelIndex() == threadChannelIndex) {
                        
							co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(200, "success", std::move(data)));

                        }
                        else {
                        
                            httpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(200, "success", std::move(data)));

                        }

                        co_return;
                };

            // -------- 路由 /api/v1/managers/stat --------
            httpHandlers["/api/v1/managers/stat"] =
                [this, httpSocketAsyncWrite, verifyAuthorization, serializeHttpResp, awaitableHttpSocketAsyncWrite](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {

                        // ========== 第一步：获取 manager 和通道索引，比较 ==========
                        auto manager = httpSocket->getWebRTCSignalManager();
                        int currentChannelIndex = manager->channelIndex;
                        bool isSameChannel = (currentChannelIndex == threadChannelIndex);  // threadChannelIndex 是 thread_local

                        // ========== 第二步：鉴权（不管同不同通道，都要验证） ==========
                        if (!verifyAuthorization(httpRequest)) {
                            if (isSameChannel) {
                                co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(403, "Forbidden", nullptr));
                            }
                            else {
                                httpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(403, "Forbidden", nullptr));
                            }
                            co_return;
                        }

                        // ========== 第三步：解析请求体（获取要查询的目标通道索引） ==========
                        unsigned char parseBuf[256];
                        boost::json::monotonic_resource parseMr(parseBuf, sizeof(parseBuf));
                        boost::json::value reqBody;
                        try {
                            reqBody = boost::json::parse(httpRequest.body(), &parseMr);
                        }
                        catch (const boost::system::system_error&) {

                            httpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                serializeHttpResp(400, "Invalid JSON body", nullptr));

                            co_return;
                        }

                        if (!reqBody.is_object()) {
                            if (isSameChannel) {
                                co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Request body must be an object", nullptr));
                            }
                            else {
                                httpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Request body must be an object", nullptr));
                            }
                            co_return;
                        }

                        auto& obj = reqBody.as_object();
                        auto it = obj.find("channelIndex");
                        if (it == obj.end() || !it->value().is_int64()) {
                            if (isSameChannel) {
                                co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Missing or invalid channelIndex", nullptr));
                            }
                            else {
                                httpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Missing or invalid channelIndex", nullptr));
                            }
                            co_return;
                        }

                        size_t targetIdx = static_cast<size_t>(it->value().as_int64());
                        WebRTCSignalServer* server = manager->webrtcSignalServer;
                        if (targetIdx >= server->getChannelNumbers()) {
                            if (isSameChannel) {
                                co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Invalid channelIndex", nullptr));
                            }
                            else {
                                httpSocketAsyncWrite(httpSocket, httpRequest.version(),
                                    serializeHttpResp(400, "Invalid channelIndex", nullptr));
                            }
                            co_return;
                        }

                        // ========== 第四步：处理请求 ==========
                        if (targetIdx == static_cast<size_t>(currentChannelIndex)) {
                            // 查询的是当前 manager 自己的通道
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

                            std::string respBody = serializeHttpResp(200, "success", std::move(targetData));
                            if (isSameChannel) {
                                co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(), std::move(respBody));
                            }
                            else {
                                httpSocketAsyncWrite(httpSocket, httpRequest.version(), std::move(respBody));
                            }
                            co_return;
                        }
                        else {
                            // 查询的是其他通道，通过 postTaskAsync 跨通道获取
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
                                                // 跨通道回写：目标 manager 在另一个通道，这里统一用非协程版本
                                                httpSocketAsyncWrite(httpSocket, version, boost::json::serialize(resp));
                                                co_return;
                                            });
                                        co_return;
                                });
                            co_return;
                        }
                };

            httpLogicHandlers["/api/v1/managers/overview"] = true;
            httpLogicHandlers["/api/v1/managers/stat"] = false;
        }

    }

}