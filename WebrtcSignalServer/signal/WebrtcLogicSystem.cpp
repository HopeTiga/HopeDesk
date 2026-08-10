#include "WebrtcLogicSystem.h"

#include <iostream>
#include <chrono>
#include <string_view>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   
#include <boost/json.hpp>

#include <absl/strings/str_format.h>

#include "WebrtcSignalServer.h"
#include "WebrtcSignalManager.h"
#include "WebrtcSignalSocket.h"
#include "HttpFilters.h"
#include "HttpSocket.h"
#include "WebrtcSignalPacket.h"

#include "../rpc/Rpc.h"
#include "../rpc/CoroRpc.h"
#include "../rpc/CoroRpcHandleImpl.h"

#include "../mysql/AsyncTransactionGuard.h"

#include "../utils/Utils.h"

namespace hope {

    namespace signal
    {

        thread_local int threadChannelIndex = -1;

        WebrtcLogicSystem::WebrtcLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues, int threshold, int exitThreshold, int asyncThreshold)
            : ioContext(ioContext)
            , channelIndex(channelIndex)
            , taskQueues(taskQueues)
        {
            webrtcMysqlManagerPools = std::make_shared<hope::mysql::WebrtcMysqlManagerPools>(ioContext);

            this->threshold.store(static_cast<uint32_t>(threshold));

            this->exitThreshold.store(static_cast<uint32_t>(exitThreshold));

            this->asyncThreshold.store(static_cast<uint32_t>(asyncThreshold));

            boost::asio::post(ioContext, [channelIndex]() {

                threadChannelIndex = channelIndex;

                });

        }

        boost::asio::io_context& WebrtcLogicSystem::getIoCompletionPorts()
        {
            return ioContext;
        }

        WebrtcLogicSystem::~WebrtcLogicSystem() {

            closeEvent();

        }

        void WebrtcLogicSystem::asyncEvent() {

            if (asyncEvents.exchange(true)) return;

            initHandlers();

            initFilters();

            initHttpHandlers();

        }

        void WebrtcLogicSystem::closeEvent() {

            if (!asyncEvents.exchange(false)) return;

            webrtcMysqlManagerPools.reset();

            webrtcHandlers.clear();

            httpHandlers.clear();

        }

        void WebrtcLogicSystem::asyncTaskExecute() {

            bool expected = false;

            if (!asyncTaskExecutes.compare_exchange_strong(expected, true)) return;

            boost::asio::co_spawn(ioContext, [webrtcLogicSystem = shared_from_this()]()mutable->boost::asio::awaitable<void> {

                while (webrtcLogicSystem->asyncEvents.load()) {

                    std::optional<AwaitableTask> optional = co_await webrtcLogicSystem->taskQueues.dequeue();

                    if (!optional.has_value()) break;

                    AwaitableTask func = std::move(optional.value());

                    if (func) {

                        boost::asio::co_spawn(webrtcLogicSystem->ioContext, [func = std::move(func)]() mutable -> boost::asio::awaitable<void> {

                            co_await func();

                            }, [](std::exception_ptr ptr) {
                                if (ptr) {
                                    try {
                                        std::rethrow_exception(ptr);
                                    }
                                    catch (const std::exception& e) {
                                        LOG_ERROR("WebrtcLogicSystem AsyncTaskExecute Task Exception: %s", e.what());
                                    }
                                }
                                });
                    }

                    if (webrtcLogicSystem->localTaskQueueSize.load() >= webrtcLogicSystem->exitThreshold.load()) {

                        LOG_WARN("WebrtcLogicSystem local queue depth %d exceeds threshold, switching to local processing", webrtcLogicSystem->localTaskQueueSize.load());

                        webrtcLogicSystem->asyncTaskExecutes.store(false);

                        break;
                    }

                    if (!webrtcLogicSystem->asyncEvents.load()) {

                        webrtcLogicSystem->asyncTaskExecutes.store(false);

                        break;

                    }

                }

                LOG_INFO("WebrtcLogicSystem AsyncTaskExecute CloseAsyncEvent");

                co_return;

                }, boost::asio::detached);

            return;

        }

        void WebrtcLogicSystem::postTaskAsync(WebrtcSignalPacket webrtcSignalPacket) {

            int type = webrtcSignalPacket.requestType;

            absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<void>(WebrtcSignalPacket)>>::iterator iterator = this->webrtcHandlers.find(type);

            if (iterator != this->webrtcHandlers.end()) {

                absl::AnyInvocable<boost::asio::awaitable<void>(WebrtcSignalPacket)>& func = iterator->second;

                if (localTaskQueueSize.load() >= threshold.load() && webrtcLogicHandlers[type]) {

                    std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket->shared_from_this();

                    bool success = taskQueues.enqueue([type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]()mutable -> boost::asio::awaitable<void> {

                        try {

                            co_await func(std::move(webrtcSignalPacket));

                        }
                        catch (...) {

                            throw;

                        }

                        co_return;

                        });

                    if (!success) {

                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%d,"state":503,"message":"webrtcSignalServer busy, please retry later"})", type));

                    }

                    return;

                }

                localTaskQueueSize.fetch_add(1);

                boost::asio::co_spawn(ioContext, [type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<void> {

                    co_await func(std::move(webrtcSignalPacket));

                    },
                    [this, type](std::exception_ptr ptr) {

                        if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

                            asyncTaskExecute();

                        }

                        if (ptr) {
                            try {

                                std::rethrow_exception(ptr);

                            }
                            catch (const std::exception& e) {

                                LOG_ERROR("WebrtcLogicSystem boost::asio::co_spawn Task: %d Exception: %s", type, e.what());

                            }
                        }
                    });

            }
            else {
                LOG_ERROR("Unknown Webrtc Request Type: %d", type);
            }
        }

        void WebrtcLogicSystem::postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest)
        {

            std::string targetUrl = httpRequest.target();

            absl::flat_hash_map<std::string, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>>::iterator iterator = this->httpHandlers.find(targetUrl);

            if (iterator != this->httpHandlers.end()) {

                LOG_INFO("Http Request: %s", targetUrl.data());

                absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>& func = iterator->second;

                if (localTaskQueueSize.load() >= threshold.load() && httpLogicHandlers[targetUrl]) {

                    unsigned int version = httpRequest.version();

                    std::shared_ptr<HttpSocket> httpSocketShared = httpSocket->shared_from_this();

                    bool success = taskQueues.enqueue([httpSocket = std::move(httpSocket), httpRequest = std::move(httpRequest), &func, this]()mutable -> boost::asio::awaitable<void> {

                        try {

                            if (!httpFilters.authorization(httpSocket, httpRequest)) {

                                boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::ok, httpRequest.version() };

                                httpResponse.set(boost::beast::http::field::content_type, "application/json");

                                httpResponse.body() = R"({"state":403,"message":"webrtcSignalServer forbidden, please check your request","data":null})";

                                httpResponse.prepare_payload();

                                httpResponse.keep_alive(httpSocket->getKeepAlive());

                                co_await httpSocket->asyncWrite(std::move(httpResponse));

                                LOG_WARN("Http Request: %s Filtered Out", httpRequest.target().data());

                                co_return;

                            }

                            co_await func(httpSocket, httpRequest);

                        }
                        catch (...) {

                            throw;

                        }

                        co_return;

                        });

                    if (!success) {

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

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRequest = std::move(httpRequest), &func, this]()mutable->boost::asio::awaitable<void> {

                    if (!httpFilters.authorization(httpSocket, httpRequest)) {

                        boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::ok, httpRequest.version() };

                        httpResponse.set(boost::beast::http::field::content_type, "application/json");

                        httpResponse.body() = R"({"state":403,"message":"webrtcSignalServer forbidden, please check your request","data":null})";

                        httpResponse.prepare_payload();

                        httpResponse.keep_alive(httpSocket->getKeepAlive());

                        co_await httpSocket->asyncWrite(std::move(httpResponse));

                        LOG_WARN("Http Request: %s Filtered Out", httpRequest.target().data());

                        co_return;

                    }

                    co_await func(httpSocket, httpRequest);

                    }, [this, targetUrl](std::exception_ptr ptr) {

                        if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

                            asyncTaskExecute();

                        }

                        if (ptr) {
                            try {

                                std::rethrow_exception(ptr);

                            }
                            catch (const std::exception& e) {

                                LOG_ERROR("WebrtcLogicSystem boost::asio::co_spawn HttpTask: %s Exception: %s", targetUrl.c_str(), e.what());

                            }
                        }
                        });

            }
            else {

                boost::asio::io_context& ioContext = httpSocket->getIoContext();

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRequest = std::move(httpRequest)]()mutable->boost::asio::awaitable<void> {

                    LOG_WARN("Http Request Not Found: %s", httpRequest.target().data());

                    boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::not_found,httpRequest.version() };

                    httpResponse.set(boost::beast::http::field::server, "WebrtcSignalServer");

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

                    }, [targetUrl](std::exception_ptr ptr) {
                        if (ptr) {
                            try {
                                std::rethrow_exception(ptr);
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("WebrtcLogicSystem boost::asio::co_spawn HttpTask: %s Exception: %s", targetUrl.c_str(), e.what());
                            }
                        }
                        });

            }

        }

        void WebrtcLogicSystem::initHandlers() {

			std::shared_ptr<WebrtcLogicSystem> webrtcLogicSystem = shared_from_this();

            std::function<boost::asio::awaitable<void>(WebrtcSignalPacket, std::string)> forwardHandler = [this](WebrtcSignalPacket webrtcSignalPacket, std::string requestTypeStr)->boost::asio::awaitable<void> {

                boost::json::object& request = webrtcSignalPacket.request;

                std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket;

                int64_t requestTypeValue = webrtcSignalPacket.requestType;

                if (!request.contains("accountId") || !request.contains("targetId")) {

                    LOG_WARN("Forward Message Missing accountId or targetId.");

                    co_return;

                }

                std::string accountId = boost::json::value_to<std::string>(request["accountId"]);

                std::string targetId = boost::json::value_to<std::string>(request["targetId"]);

                std::shared_ptr<WebrtcSignalSocket> targetSocket = nullptr;

                {
                    absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalPacket.webrtcSignalManager->webrtcSocketMap.find(targetId);

                    if (iterator != webrtcSignalPacket.webrtcSignalManager->webrtcSocketMap.end()) {

                        targetSocket = iterator->second;

                    }

                }

                if (!targetSocket) {

                    absl::node_hash_map<std::string, int>::iterator iterator = webrtcSignalPacket.webrtcSignalSocket->actorMappingIndex.find(targetId);

                    int index = 0;

                    if (iterator == webrtcSignalPacket.webrtcSignalSocket->actorMappingIndex.end()) {

                        index = -1;

                    }
                    else {

                        index = iterator->second;

                    }

                    int mapChannelIndex = webrtcSignalPacket.webrtcSignalManager->hasher(targetId.c_str()) % webrtcSignalPacket.webrtcSignalManager->hashSize;

                    int channelIndex = webrtcSignalPacket.webrtcSignalManager->getChannelIndex();

                    if (index == -1) {

                        if (mapChannelIndex == channelIndex) {

                            absl::node_hash_map<std::string, WebrtcSignalManager::ActorMapping>::iterator indexIterator = webrtcSignalPacket.webrtcSignalManager->actorSocketMappingIndex.find(targetId);

                            if (indexIterator != webrtcSignalPacket.webrtcSignalManager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = indexIterator->second.channelIndex;

                                webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTask(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                    absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                    if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                        request["state"] = 200;

                                        request["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        int targetChannelIndex = webrtcSignalManager->channelIndex;

                                        webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                            return;

                                            });

                                        return;

                                    }
                                    else {

                                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                        LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        return;

                                    }
                                    });

                                co_return;
                            }
                            else {

                                webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                co_return;

                            }

                        }

                        webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTask(mapChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(webrtcSignalPacket.request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                            absl::node_hash_map<std::string, WebrtcSignalManager::ActorMapping>::iterator indexIterator = webrtcSignalManager->actorSocketMappingIndex.find(targetId);

                            if (indexIterator != webrtcSignalManager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = indexIterator->second.channelIndex;

                                if (targetChannelIndex == webrtcSignalManager->getChannelIndex()) {

                                    absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                    if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                        request["state"] = 200;

                                        request["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                            return;

                                            });

                                        return;

                                    }
                                    else {

                                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                        LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        return;

                                    }

                                }

                                webrtcSignalManager->webrtcSignalServer->postTask(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                    absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                    if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                        request["state"] = 200;

                                        request["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        int targetChannelIndex = webrtcSignalManager->channelIndex;

                                        webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                            return;

                                            });

                                        return;

                                    }
                                    else {

                                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                        LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        return;

                                    }
                                    });
                            }
                            else {

                                webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                return;

                            }
                            });
                    }
                    else {

                        webrtcSignalPacket.webrtcSignalManager->webrtcSignalServer->postTask(index, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), mapChannelIndex = std::move(mapChannelIndex), request = std::move(webrtcSignalPacket.request), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                            absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                            if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                request["state"] = 200;

                                request["message"] = "webrtcSignalServer forward";

                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                return;

                            }
                            else {

                                if (mapChannelIndex == webrtcSignalManager->getChannelIndex()) {

                                    absl::node_hash_map<std::string, WebrtcSignalManager::ActorMapping>::iterator indexIterator = webrtcSignalManager->actorSocketMappingIndex.find(targetId);

                                    if (indexIterator != webrtcSignalManager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = indexIterator->second.channelIndex;

                                        webrtcSignalManager->webrtcSignalServer->postTask(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                            if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                                request["state"] = 200;

                                                request["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                int targetChannelIndex = webrtcSignalManager->channelIndex;

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                                    return;

                                                    });

                                                return;

                                            }
                                            else {

                                                webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    absl::node_hash_map<std::string, int>::iterator routeIterator = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                                    if (routeIterator != webrtcSignalSocket->actorMappingIndex.end() && routeIterator->second == index) {

                                                        webrtcSignalSocket->actorMappingIndex.erase(routeIterator);

                                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                                    }

                                                    return;

                                                    });

                                                return;

                                            }
                                            });

                                        return;
                                    }
                                    else {

                                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                        LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            absl::node_hash_map<std::string, int>::iterator routeIterator = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                            if (routeIterator != webrtcSignalSocket->actorMappingIndex.end() && routeIterator->second == index) {

                                                webrtcSignalSocket->actorMappingIndex.erase(routeIterator);

                                                LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                            }

                                            return;

                                            });

                                        return;

                                    }

                                }

                                webrtcSignalManager->webrtcSignalServer->postTask(mapChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                    absl::node_hash_map<std::string, WebrtcSignalManager::ActorMapping>::iterator indexIterator = webrtcSignalManager->actorSocketMappingIndex.find(targetId);

                                    if (indexIterator != webrtcSignalManager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = indexIterator->second.channelIndex;

                                        if (targetChannelIndex == webrtcSignalManager->getChannelIndex()) {

                                            absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                            if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                                request["state"] = 200;

                                                request["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                                    return;

                                                    });

                                                return;

                                            }
                                            else {

                                                webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    absl::node_hash_map<std::string, int>::iterator routeIterator = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                                    if (routeIterator != webrtcSignalSocket->actorMappingIndex.end() && routeIterator->second == index) {

                                                        webrtcSignalSocket->actorMappingIndex.erase(routeIterator);

                                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                                    }

                                                    return;

                                                    });

                                                return;

                                            }

                                        }

                                        webrtcSignalManager->webrtcSignalServer->postTask(targetChannelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), channelIndex = std::move(channelIndex), request = std::move(request), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

                                            if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

                                                request["state"] = 200;

                                                request["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(request)));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                int targetChannelIndex = webrtcSignalManager->channelIndex;

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), targetChannelIndex, targetId = std::move(targetId)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    webrtcSignalSocket->actorMappingIndex[targetId] = targetChannelIndex;

                                                    return;

                                                    });

                                                return;

                                            }
                                            else {

                                                webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                                LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                                    absl::node_hash_map<std::string, int>::iterator routeIterator = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                                    if (routeIterator != webrtcSignalSocket->actorMappingIndex.end() && routeIterator->second == index) {

                                                        webrtcSignalSocket->actorMappingIndex.erase(routeIterator);

                                                        LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                                    }

                                                    return;

                                                    });

                                                return;

                                            }
                                            });
                                    }
                                    else {

                                        webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":404,"message":"TargetId is not register"})", static_cast<long long>(requestTypeValue)));

                                        LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        webrtcSignalManager->webrtcSignalServer->postTask(channelIndex, [webrtcSignalSocket = std::move(webrtcSignalSocket), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable {

                                            absl::node_hash_map<std::string, int>::iterator routeIterator = webrtcSignalSocket->actorMappingIndex.find(targetId);

                                            if (routeIterator != webrtcSignalSocket->actorMappingIndex.end() && routeIterator->second == index) {

                                                webrtcSignalSocket->actorMappingIndex.erase(routeIterator);

                                                LOG_DEBUG("Stale route cache cleared for: %s -> %s", accountId.c_str(), targetId.c_str());

                                            }

                                            return;

                                            });

                                        return;

                                    }

                                    });
                            }

                            return;

                            });
                    }

                    co_return;

                }

                request["state"] = 200;

                request["message"] = "webrtcSignalServer forward";

                targetSocket->asyncWrite(boost::json::serialize(std::move(request)));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                co_return;

                };

            // ==================== Handlers 1-4 ====================
            webrtcHandlers[1] = [this, forwardHandler](WebrtcSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(webrtcSignalPacket), "REQUEST"); };

            webrtcHandlers[3] = [this, forwardHandler](WebrtcSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> { co_await forwardHandler(std::move(webrtcSignalPacket), "STOP_REMOTE"); };

            webrtcHandlers[6] = [this, forwardHandler](WebrtcSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(webrtcSignalPacket), "CLOSE_SYSTEM");
                };

            webrtcHandlers[7] = [this, forwardHandler](WebrtcSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(webrtcSignalPacket), "SYSTEM_READLY");
                };

            webrtcHandlers[9] = [this, forwardHandler](WebrtcSignalPacket webrtcSignalPacket)->boost::asio::awaitable<void> {

                hope::rpc::CoroRpc* coroRpc = hope::rpc::CoroRpc::getInstance();

                if (!coroRpc->isOpen()) {

                    LOG_WARN("CoroRpc is not accepted yet, request aborted");

                    co_return;

                }

                std::shared_ptr<boost::asio::steady_timer> sharedTimer = std::make_shared<boost::asio::steady_timer>(ioContext);

                sharedTimer->expires_after(std::chrono::milliseconds(3000));

                boost::json::object& request = webrtcSignalPacket.request;

                if (!request.contains("forwardPacket")) {

                    LOG_WARN("Forward Message Missing forwardPacket.");

                    webrtcSignalPacket.webrtcSignalSocket->asyncWrite(absl::StrFormat(R"({"requestType":%lld,"state":400,"message":"Forward Message Missing forwardPacket"})", static_cast<long long>(webrtcSignalPacket.requestType)));

                    co_return;

                }

                std::string forwardPacketJson = boost::json::serialize(request["forwardPacket"].as_object());

                std::shared_ptr<RpcForwardResponse> rpcForwardResponse = std::make_shared<RpcForwardResponse>();

                coroRpc->asyncAwait(
                    [](hope::rpc::CoroRpc* rpc, std::shared_ptr<boost::asio::steady_timer> timer,
                       std::string packet, std::shared_ptr<RpcForwardResponse> resp)
                    -> async_simple::coro::Lazy<void> {

                        std::string targetHost = "127.0.0.1:" + std::to_string(rpc->coroRpcServerConfig.port);

                        auto result = co_await rpc->asyncRpcRequest(
                            targetHost,
                            [packet = std::move(packet), targetHost](coro_rpc::coro_rpc_client& client)mutable
                            -> async_simple::coro::Lazy<coro_rpc::rpc_result<RpcForwardResponse>> {

                                RpcForward rpcForward(0, std::move(packet));

                                co_return co_await client.call<&hope::rpc::CoroRpcHandleImpl::requestForward>(rpcForward);

                            });
                        if (!result) {

                            std::error_code connectError = std::make_error_code(result.error());

                            LOG_WARN("RpcForward Connect Failed, Error=%d (%s)", static_cast<int>(result.error()), connectError.message().c_str());

                        }
                        else if (!result.value()) {

                            LOG_WARN("RpcForward CoroRpc Call Failed");

                        }
                        else {

                            *resp = result.value().value();

                        }

                        timer->cancel();

                        co_return;

                    },
                    coroRpc, sharedTimer, std::move(forwardPacketJson), rpcForwardResponse);

                auto [waitEc] = co_await sharedTimer->async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));

                if (waitEc != boost::asio::error::operation_aborted) {

                    LOG_WARN("RpcForward wait timeout (3s), response not received");

                    co_return;
                }

                LOG_INFO("RpcForwardResponse State:%d Message:%s", rpcForwardResponse->state, rpcForwardResponse->message.c_str());

                co_return;

                };

            webrtcLogicHandlers[1] = false;

            webrtcLogicHandlers[3] = false;

            webrtcLogicHandlers[6] = false;

            webrtcLogicHandlers[7] = false;

            webrtcLogicHandlers[9] = false;

        }

        void WebrtcLogicSystem::initFilters()
        {

            httpFilters.addRule("/api/v1/managers/login");

            httpFilters.addFilter([](std::shared_ptr<HttpSocket> httpSocket, const boost::beast::http::request<boost::beast::http::string_body>& httpRequest) -> bool {

                boost::beast::http::request<boost::beast::http::string_body>::const_iterator iterator = httpRequest.find(boost::beast::http::field::authorization);

                if (iterator == httpRequest.end()) return false;

                std::string_view authView{ iterator->value().data(), iterator->value().size() };

                if (authView.size() < 7 || authView.substr(0, 7) != "Bearer ") return false;

                return authView.substr(7) == "913140924@qq.com";

                });

        }

        void WebrtcLogicSystem::initHttpHandlers()
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
                                LOG_ERROR("WebrtcLogicSystem boost::asio::co_spawn HttpTask Response Exception: %s", e.what());
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

            // -------- 路由 /api/v1/managers/overview --------
            httpHandlers["/api/v1/managers/overview"] =
                [this, httpSocketAsyncWrite,serializeHttpResp, awaitableHttpSocketAsyncWrite](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {

                        WebrtcSignalServer* server = httpSocket->getWebrtcSignalManager()->webrtcSignalServer;

                        boost::json::object data;

                        data["totalManagers"] = server->getChannelNumbers();

                        LOG_INFO("channelIndex:%d threadChannelIndex:%d", httpSocket->getWebrtcSignalManager()->getChannelIndex(), threadChannelIndex);

                        if (httpSocket->getWebrtcSignalManager()->getChannelIndex() == threadChannelIndex) {

                            co_await awaitableHttpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(200, "success", std::move(data)));

                        }
                        else {

                            httpSocketAsyncWrite(httpSocket, httpRequest.version(), serializeHttpResp(200, "success", std::move(data)));

                        }

                        co_return;
                };

            // -------- 路由 /api/v1/managers/stat --------
            httpHandlers["/api/v1/managers/stat"] =
                [this, httpSocketAsyncWrite, serializeHttpResp,awaitableHttpSocketAsyncWrite](
                    std::shared_ptr<HttpSocket> httpSocket,
                    boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {

                        auto manager = httpSocket->getWebrtcSignalManager();

                        int currentChannelIndex = manager->channelIndex;

                        bool isSameChannel = (currentChannelIndex == threadChannelIndex);  // threadChannelIndex 是 thread_local

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
                        WebrtcSignalServer* server = manager->webrtcSignalServer;
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
                                sInfo["isRegistered"] = true;
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
                                    std::shared_ptr<WebrtcSignalManager> targetManager) mutable -> boost::asio::awaitable<void> {
                                        boost::json::storage_ptr sp = boost::json::make_shared_resource<boost::json::monotonic_resource>();
                                        boost::json::array socketList(sp);
                                        socketList.reserve(targetManager->webrtcSocketMap.size());
                                        for (auto const& [accountId, socketPtr] : targetManager->webrtcSocketMap) {
                                            boost::json::object sInfo(sp);
                                            sInfo["accountId"] = accountId;
                                            sInfo["remoteAddr"] = socketPtr->getRemoteAddress();
                                            sInfo["sessionId"] = socketPtr->getSessionId();
                                            sInfo["isRegistered"] = true;
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
                                            httpSocketAsyncWrite](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager) mutable -> boost::asio::awaitable<void> {
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
                        }
                };

            httpLogicHandlers["/api/v1/managers/overview"] = true;

            httpLogicHandlers["/api/v1/managers/stat"] = false;

        }

    }

}
