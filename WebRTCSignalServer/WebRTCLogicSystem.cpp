#include "WebRTCLogicSystem.h"
#include "WebRTCSignalServer.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "HttpSocket.h"
#include "WebRTCSignalData.h"
#include "WebRTCMysqlManagerPools.h"


#include <iostream>
#include <chrono>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include <boost/json.hpp>

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

        void WebRTCLogicSystem::postTaskAsync(std::shared_ptr<WebRTCSignalData> data) {

            int type = data->webrtcSignalRequest.requestType.value();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>)> func = this->webrtcHandlers[type];

                taskQueueSize.fetch_add(1);

                if (taskQueueSize.load() >= threshold.load() && localTaskQueueSize.load() >= threshold.load() && webrtcLogicHandlers[type]) {

                    std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = data->webrtcSignalSocket->shared_from_this();

                    bool success = taskQueues.enqueue([this, type, func, data = std::move(data)]()mutable -> boost::asio::awaitable<void> {

                        try {

                            co_await func(data);

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

                        glz::obj response{ "responseType", type, "state", 503, "message", "webrtcSignalServer busy, please retry later", "data", nullptr };

                        std::string responseBuf;

                        glz::write_json(response, responseBuf);

                        webrtcSignalSocket->asyncWrite(std::move(responseBuf));

                    }

                    return;

                }

                localTaskQueueSize.fetch_add(1);

                boost::asio::co_spawn(ioContext, [this, type, func, data]() mutable -> boost::asio::awaitable<void> {

                    co_await func(data);

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

                std::function<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)> func = httpHandlers[targetUrl];

                taskQueueSize.fetch_add(1);

                if (taskQueueSize.load() >= threshold.load() && localTaskQueueSize.load() >= threshold.load() && httpLogicHandlers[targetUrl]) {

                    unsigned int version = httpRequest.version();

                    std::shared_ptr<HttpSocket> httpSocketShared = httpSocket->shared_from_this();

                    bool success = taskQueues.enqueue([this, httpSocket = std::move(httpSocket), httpRquest = std::move(httpRequest), func = std::move(func)]()mutable -> boost::asio::awaitable<void> {

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

                            glz::obj body{ "state", 503, "message", "webrtcSignalServer busy, please retry later", "data", nullptr };

                            boost::beast::http::response<boost::beast::http::string_body> res{ boost::beast::http::status::ok, version };

                            res.set(boost::beast::http::field::content_type, "application/json");

                            glz::write_json(body, res.body());

                            res.prepare_payload();

                            res.keep_alive(httpSocket->getKeepAlive());

                            co_await httpSocket->asyncWrite(std::move(res));

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

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRquest = std::move(httpRequest), func = std::move(func)]()mutable->boost::asio::awaitable<void> {

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

                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn HttpTask: %d Exception: %s", targetUrl, e.what());

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

                    const glz::obj responseBody{ "message", "The requested resource was not found on this server.", "state", 404.0, "data", nullptr };

                    glz::write_json(responseBody, httpResponse.body());

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
                                LOG_ERROR("WebRTCLogicSystem boost::asio::co_spawn HttpTask: %d Exception: %s", targetUrl, e.what());
                            }
                        }
                        });

            }

        }

        void WebRTCLogicSystem::initHandlers() {

            // ==================== Forward Handler ====================
            std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::string)> forwardHandler = [this](std::shared_ptr<WebRTCSignalData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {

                WebRTCSignalRequest webrtcSignalRequest = data->webrtcSignalRequest;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                int64_t requestTypeValue = webrtcSignalRequest.requestType.value();

                if (!webrtcSignalRequest.dynamicData.contains("accountId") || !webrtcSignalRequest.dynamicData.contains("targetId")) {

                    LOG_WARN("Forward Message Missing accountId or targetId.");

                    co_return;

                }

                std::string accountId = webrtcSignalRequest.getString("accountId");

                std::string targetId = webrtcSignalRequest.getString("targetId");

                std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

                {
                    auto it = data->webrtcSignalManager->webrtcSocketMap.find(targetId);

                    if (it != data->webrtcSignalManager->webrtcSocketMap.end()) {

                        targetSocket = it->second;

                    }

                }

                if (!targetSocket) {

                    int index = data->webrtcSignalSocket->actorMappingIndex[targetId];

                    auto webrtcSignalServer = data->webrtcSignalManager->webrtcSignalServer;

                    int mapChannelIndex = data->webrtcSignalManager->hasher(targetId) % data->webrtcSignalManager->hashSize;

                    int channelIndex = data->webrtcSignalManager->getChannelIndex();

                    if (index == -1) {

                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), webrtcSignalRequest = std::move(data->webrtcSignalRequest), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                if (targetChannelIndex == manager->getChannelIndex()) {

                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                        webrtcSignalRequest.setInt("state", 200);

                                        webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                                        std::string response;

                                        glz::write_json(webrtcSignalRequest, response);

                                        targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                        targetWebrtcSignalSocket->asyncWrite(std::move(response));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", std::string("TargetId is not register") };

                                            std::string response = glz::write_json(forwardObj).value_or("{}");

                                            webrtcSignalSocket->asyncWrite(std::move(response));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }

                                    co_return;

                                }

                                manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), webrtcSignalRequest = std::move(webrtcSignalRequest), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                        webrtcSignalRequest.setInt("state", 200);

                                        webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                                        std::string response;

                                        glz::write_json(webrtcSignalRequest, response);

                                        targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                        targetWebrtcSignalSocket->asyncWrite(std::move(response));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", std::string("TargetId is not register") };

                                            std::string response = glz::write_json(forwardObj).value_or("{}");

                                            webrtcSignalSocket->asyncWrite(std::move(response));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    });
                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", std::string("TargetId is not register") };

                                    std::string response = glz::write_json(forwardObj).value_or("{}");

                                    webrtcSignalSocket->asyncWrite(std::move(response));

                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                    co_return;

                                    });

                                co_return;

                            }
                            });
                    }
                    else {

                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(index, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), mapChannelIndex = std::move(mapChannelIndex), webrtcSignalRequest = std::move(data->webrtcSignalRequest), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                webrtcSignalRequest.setInt("state", 200);

                                webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                                std::string response;

                                glz::write_json(webrtcSignalRequest, response);

                                targetWebrtcSignalSocket->asyncWrite(std::move(response));

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


                                manager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), webrtcSignalRequest = std::move(webrtcSignalRequest), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                        if (targetChannelIndex == manager->getChannelIndex()) {

                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                                webrtcSignalRequest.setInt("state", 200);

                                                webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                                                std::string response;

                                                glz::write_json(webrtcSignalRequest, response);

                                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                                targetWebrtcSignalSocket->asyncWrite(std::move(response));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", std::string("TargetId is not register") };

                                                    std::string response = glz::write_json(forwardObj).value_or("{}");

                                                    webrtcSignalSocket->asyncWrite(std::move(response));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }

                                            co_return;

                                        }

                                        manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), webrtcSignalRequest = std::move(webrtcSignalRequest), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                                webrtcSignalRequest.setInt("state", 200);

                                                webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                                                std::string response;

                                                glz::write_json(webrtcSignalRequest, response);

                                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                                targetWebrtcSignalSocket->asyncWrite(std::move(response));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                co_return;

                                            }
                                            else {

                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", "TargetId is not register" };

                                                    std::string response = glz::write_json(forwardObj).value_or("{}");

                                                    webrtcSignalSocket->asyncWrite(std::move(response));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            });
                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            glz::obj forwardObj{ "requestType", requestTypeValue, "state", 404.0, "message", "TargetId is not register" };

                                            std::string response = glz::write_json(forwardObj).value_or("{}");

                                            webrtcSignalSocket->asyncWrite(std::move(response));

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

                webrtcSignalRequest.setInt("state", 200);

                webrtcSignalRequest.setString("message", "webrtcSignalServer forward");

                std::string response;

                glz::write_json(webrtcSignalRequest, response);

                targetSocket->asyncWrite(std::move(response));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                };


            // ==================== Handler 0: REGISTER ====================
            webrtcHandlers[0] = [this](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                WebRTCSignalRequest& webrtcSignalRequest = data->webrtcSignalRequest;

                if (!webrtcSignalRequest.dynamicData.contains("accountId")) {

                    glz::generic response{ {"requestType", webrtcSignalRequest.requestType.value()}, {"state", 400.0}, {"message", std::string("Missing accountId in registration request")} };

                    std::string responseStr = glz::write_json(response).value_or("{}");

                    data->webrtcSignalSocket->asyncWrite(std::move(responseStr));

                    LOG_WARN("Registration Failed: Missing accountId");

                    co_return;

                }

                std::string accountId = webrtcSignalRequest.getString("accountId");

                data->webrtcSignalSocket->setAccountId(accountId);

                data->webrtcSignalSocket->setRegistered(true);

                data->webrtcSignalManager->webrtcSocketMap[accountId] = data->webrtcSignalSocket;

                glz::generic response{ {"requestType", webrtcSignalRequest.requestType.value()}, {"state", 200.0}, {"message", std::string("Register Successful")} };

                std::string responseStr = glz::write_json(response).value_or("{}");

                data->webrtcSignalSocket->asyncWrite(std::move(responseStr));

                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountId.c_str(), data->webrtcSignalManager->channelIndex);

                int mapChannelIndex = data->webrtcSignalManager->hasher(accountId) % data->webrtcSignalManager->hashSize;

                data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [managers = data->webrtcSignalManager->shared_from_this(), accountId, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->actorSocketMappingIndex[accountId] = managers->channelIndex;

                    co_return;

                    });

                co_return;

                };


            // ==================== Handlers 1-4 ====================
            webrtcHandlers[1] = [this, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "REQUEST"); };
            webrtcHandlers[2] = [this, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "RESTART"); };
            webrtcHandlers[3] = [this, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(data), "STOPREMOTE"); };

            webrtcHandlers[4] = [this](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {

                std::string accountId = data->webrtcSignalSocket->getAccountId();

                std::string sessionId = data->webrtcSignalSocket->getSessionId();

                if (!accountId.empty()) {

                    data->webrtcSignalManager->removeConnection(accountId, sessionId);

                }

                co_return;

                };

            webrtcHandlers[6] = [this, forwardHandler](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "CLOSESYSTEM");
                };

            webrtcHandlers[7] = [this, forwardHandler](std::shared_ptr<hope::core::WebRTCSignalData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "SYSTEMREADLY");
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

            std::function<void(std::shared_ptr<HttpSocket>, unsigned, std::string)> httpSocketAsyncWrite = [this](std::shared_ptr<HttpSocket> httpSocket, unsigned version, std::string body) {

                boost::asio::io_context& ioContext = httpSocket->getIoContext();

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), version, body = std::move(body)]()mutable->boost::asio::awaitable<void> {

                    boost::beast::http::response<boost::beast::http::string_body> res{ boost::beast::http::status::ok, version };

                    res.set(boost::beast::http::field::content_type, "application/json");

                    res.body() = body;

                    res.prepare_payload();

                    res.keep_alive(httpSocket->getKeepAlive());

                    co_await httpSocket->asyncWrite(std::move(res));

                    co_return;

                    }, [this](std::exception_ptr ptr) {
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

            std::function<void(std::shared_ptr<HttpSocket>, unsigned, int, std::string)> httpSocketAsyncWriteError = [this, httpSocketAsyncWrite](std::shared_ptr<HttpSocket> httpSocket, unsigned version, int code, std::string msg) {

                glz::generic resp{ {"state", static_cast<double>(code)}, {"message", std::move(msg)}, {"data", nullptr} };

                httpSocketAsyncWrite(httpSocket, version, std::move(glz::write_json(resp).value_or("{}")));

                };

            std::function<bool(const boost::beast::http::request<boost::beast::http::string_body>&)> verifyAuthorization = [this, httpSocketAsyncWrite, httpSocketAsyncWriteError](const boost::beast::http::request<boost::beast::http::string_body>& req) {

                try {

                    auto it = req.find(boost::beast::http::field::authorization);

                    if (it == req.end()) return false;

                    std::string authHeader(it->value());

                    if (authHeader.size() < 7 || authHeader.substr(0, 7) != "Bearer ") return false;

                    std::string token = authHeader.substr(7);

                    if (token == "913140924@qq.com") return true;

                    return false;

                }
                catch (std::exception& e) {

                    LOG_ERROR("Authorization Verify Error:%s", e.what());

                    return false;
                }
                };

            httpHandlers["/api/v1/managers/overview"] = [this, httpSocketAsyncWrite, httpSocketAsyncWriteError, verifyAuthorization](std::shared_ptr<HttpSocket> httpSocket, auto httpRequest) mutable -> boost::asio::awaitable<void> {

                if (!verifyAuthorization(httpRequest)) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 403, "Forbidden");

                    co_return;

                }

                WebRTCSignalServer* server = httpSocket->getWebRTCSignalManager()->webrtcSignalServer;

                glz::generic data{ {"totalManagers", static_cast<double>(server->getChannelNumbers())} };

                glz::generic resp{ {"state", 200.0}, {"message", std::string("success")}, {"data", std::move(data)} };

                httpSocketAsyncWrite(httpSocket, httpRequest.version(), std::move(glz::write_json(resp).value_or("{}")));

                co_return;

                };


            httpHandlers["/api/v1/managers/stat"] = [this, httpSocketAsyncWrite, httpSocketAsyncWriteError, verifyAuthorization](std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {

                if (!verifyAuthorization(httpRequest)) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 403, "Forbidden");

                    co_return;

                }

                auto reqBodyResult = glz::read_json<glz::generic>(httpRequest.body());

                if (!reqBodyResult || !reqBodyResult->is_object() || !reqBodyResult->contains("channelIndex")) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Missing channelIndex");

                    co_return;

                }

                size_t targetIdx = static_cast<size_t>((*reqBodyResult)["channelIndex"].as<double>());

                if (targetIdx >= httpSocket->getWebRTCSignalManager()->webrtcSignalServer->getChannelNumbers()) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Invalid channelIndex");

                    co_return;

                }

                int currentChannelIndex = httpSocket->getWebRTCSignalManager()->channelIndex;

                WebRTCSignalServer* server = httpSocket->getWebRTCSignalManager()->webrtcSignalServer;

                if (targetIdx == currentChannelIndex) {

                    glz::generic::array_t socketList;

                    for (auto const& [accountId, socketPtr] : httpSocket->getWebRTCSignalManager()->webrtcSocketMap) {

                        glz::generic sInfo{ {"accountId", accountId}, {"remoteAddr", socketPtr->getRemoteAddress()}, {"sessionId", socketPtr->getSessionId()}, {"isRegistered", socketPtr->getRegistered()}, {"cachedRouteCount", static_cast<double>(socketPtr->actorMappingIndex.size())} };

                        socketList.emplace_back(std::move(sInfo));

                    }

                    glz::generic targetData{ {"channelIndex", static_cast<double>(httpSocket->getWebRTCSignalManager()->channelIndex)}, {"totalSockets", static_cast<double>(httpSocket->getWebRTCSignalManager()->webrtcSocketMap.size())}, {"sockets", std::move(socketList)} };

                    glz::generic resp{ {"state", 200.0}, {"message", std::string("success")}, {"data", std::move(targetData)} };

                    httpSocketAsyncWrite(httpSocket, httpRequest.version(), std::move(glz::write_json(resp).value_or("{}")));

                    co_return;

                }

                server->postTaskAsync(targetIdx, [this, httpSocket = httpSocket->shared_from_this(), version = httpRequest.version(), currentChannelIndex, httpSocketAsyncWrite](std::shared_ptr<WebRTCSignalManager> targetManager) mutable -> boost::asio::awaitable<void> {

                    glz::generic::array_t socketList;

                    for (auto const& [accountId, socketPtr] : targetManager->webrtcSocketMap) {

                        glz::generic sInfo{ {"accountId", accountId}, {"remoteAddr", socketPtr->getRemoteAddress()}, {"sessionId", socketPtr->getSessionId()}, {"isRegistered", socketPtr->getRegistered()}, {"cachedRouteCount", static_cast<double>(socketPtr->actorMappingIndex.size())} };

                        socketList.emplace_back(std::move(sInfo));

                    }

                    glz::generic targetData{ {"channelIndex", static_cast<double>(targetManager->channelIndex)}, {"totalSockets", static_cast<double>(targetManager->webrtcSocketMap.size())}, {"sockets", std::move(socketList)} };

                    targetManager->webrtcSignalServer->postTaskAsync(currentChannelIndex, [this, httpSocket = httpSocket->shared_from_this(), version, targetData = std::move(targetData), httpSocketAsyncWrite](std::shared_ptr<WebRTCSignalManager> manager) mutable -> boost::asio::awaitable<void> {

                        glz::generic resp{ {"state", 200.0}, {"message", std::string("success")}, {"data", std::move(targetData)} };

                        httpSocketAsyncWrite(httpSocket, version, std::move(glz::write_json(resp).value_or("{}")));

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