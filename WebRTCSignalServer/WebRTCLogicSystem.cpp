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

#include "AsyncTransactionGuard.h"

#include "Utils.h"


namespace hope {

    namespace core
    {

        WebRTCLogicSystem::WebRTCLogicSystem(boost::asio::io_context& ioContext, int channelIndex) :ioContext(ioContext), channelIndex(channelIndex)
        {
            webrtcMysqlManagerPools = std::make_shared<hope::mysql::WebRTCMysqlManagerPools>(ioContext);
        }

        boost::asio::io_context& WebRTCLogicSystem::getIoCompletePorts()
        {
            return ioContext;
        }

        WebRTCLogicSystem::~WebRTCLogicSystem() {

            webrtcMysqlManagerPools.reset();

            webrtcHandlers.clear();

        }

        void WebRTCLogicSystem::postTaskAsync(std::shared_ptr<WebRTCSignalData> data) {

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

        void WebRTCLogicSystem::postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest)
        {
            std::string targetUrl = httpRequest.target();

            if (httpHandlers.find(targetUrl) != httpHandlers.end()) {

                LOG_INFO("Http Request: %s", targetUrl.data());

                std::function<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)> func = httpHandlers[targetUrl];

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRquest = std::move(httpRequest), func = std::move(func)]()mutable->boost::asio::awaitable<void> {

                    co_await func(httpSocket, httpRquest);

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
            else {

                boost::asio::io_context& ioContext = httpSocket->getIoContext();

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), httpRequest = std::move(httpRequest)]()mutable->boost::asio::awaitable<void> {

                    LOG_WARN("Http Request Not Found: %s", httpRequest.target().data());

                    boost::beast::http::response<boost::beast::http::string_body> httpResponse{ boost::beast::http::status::not_found,httpRequest.version() };

                    httpResponse.set(boost::beast::http::field::server, "WebRTCSignalServer");

                    httpResponse.set(boost::beast::http::field::content_type, "application/json");

                    httpResponse.keep_alive(httpSocket->getKeepAlive());

                    boost::json::object responseBody;

                    responseBody["message"] = "The requested resource was not found on this server.";

                    responseBody["state"] = 404;

                    responseBody["data"] = nullptr;

                    httpResponse.body() = boost::json::serialize(responseBody);

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

            auto self = shared_from_this();

            // ==================== Forward Handler ====================
            std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalData>, std::string)> forwardHandler = [this](std::shared_ptr<WebRTCSignalData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {
                boost::json::object message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;
                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountId") || !message.contains("targetId")) {
                    LOG_WARN("Forward Message Missing accountId or targetId.");
                    co_return;
                }

                std::string accountId = message["accountId"].as_string().c_str();
                std::string targetId = message["targetId"].as_string().c_str();
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

                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), message = std::move(message), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), message = std::move(message), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                        std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                        boost::json::object forwardMessage = message;

                                        forwardMessage["state"] = 200;

                                        forwardMessage["message"] = "webrtcSignalServer forward";

                                        targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                        targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

                                        LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                        co_return;

                                    }
                                    else {

                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            boost::json::object response;

                                            response["requestType"] = requestTypeValue;

                                            response["state"] = 404;

                                            response["message"] = "TargetId is not register";

                                            webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                            LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                            co_return;

                                            });

                                        co_return;

                                    }
                                    });
                            }
                            else {

                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    boost::json::object response;

                                    response["requestType"] = requestTypeValue;

                                    response["state"] = 404;

                                    response["message"] = "TargetId is not register";

                                    webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                    co_return;

                                    });

                                co_return;

                            }
                            });
                    }
                    else {

                        data->webrtcSignalManager->webrtcSignalServer->postTaskAsync(index, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), mapChannelIndex = std::move(mapChannelIndex), message = std::move(message), requestTypeStr = std::move(requestTypeStr), requestTypeValue = std::move(requestTypeValue), accountId = std::move(accountId), targetId = std::move(targetId), index](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                boost::json::object forwardMessage = message;

                                forwardMessage["state"] = 200;

                                forwardMessage["message"] = "webrtcSignalServer forward";

                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

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


                                manager->webrtcSignalServer->postTaskAsync(mapChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), message = std::move(message), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                    if (manager->actorSocketMappingIndex.find(targetId) != manager->actorSocketMappingIndex.end()) {

                                        int targetChannelIndex = manager->actorSocketMappingIndex[targetId];

                                        manager->webrtcSignalServer->postTaskAsync(targetChannelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), channelIndex = std::move(channelIndex), message = std::move(message), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            if (manager->webrtcSocketMap.find(targetId) != manager->webrtcSocketMap.end()) {

                                                std::shared_ptr<WebRTCSignalSocket> targetWebrtcSignalSocket = manager->webrtcSocketMap[targetId];

                                                boost::json::object forwardMessage = message;

                                                forwardMessage["state"] = 200;

                                                forwardMessage["message"] = "webrtcSignalServer forward";

                                                targetWebrtcSignalSocket->actorMappingIndex[accountId] = channelIndex;

                                                targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(forwardMessage));

                                                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                co_return;

                                            }
                                            else {
                                                manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                                    boost::json::object response;

                                                    response["requestType"] = requestTypeValue;

                                                    response["state"] = 404;

                                                    response["message"] = "TargetId is not register";

                                                    webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                                                    LOG_WARN("Request forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());

                                                    co_return;

                                                    });

                                                co_return;

                                            }
                                            });
                                    }
                                    else {
                                        manager->webrtcSignalServer->postTaskAsync(channelIndex, [webrtcSignalSocket = webrtcSignalSocket->shared_from_this(), requestTypeValue = std::move(requestTypeValue), requestTypeStr = std::move(requestTypeStr), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<WebRTCSignalManager> manager)mutable->boost::asio::awaitable<void> {

                                            boost::json::object response;

                                            response["requestType"] = requestTypeValue;

                                            response["state"] = 404;

                                            response["message"] = "TargetId is not register";

                                            webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

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

                boost::json::object forwardMessage = message;

                forwardMessage["state"] = 200;

                forwardMessage["message"] = "webrtcSignalServer forward";

                targetSocket->asyncWrite(boost::json::serialize(forwardMessage));

                LOG_INFO("Request forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());
                };


            // ==================== Handler 0: REGISTER ====================
            webrtcHandlers[0] = [this](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                boost::json::object response;

                response["requestType"] = message["requestType"].as_int64();

                if (!message.contains("accountId")) {

                    response["state"] = 400;
                    response["message"] = "Missing accountId in registration request";
                    data->webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

                    LOG_WARN("Registration Failed: Missing accountId");
                    co_return;

                }

                std::string accountId = message["accountId"].as_string().c_str();

                data->webrtcSignalSocket->setAccountId(accountId);

                data->webrtcSignalSocket->setRegistered(true);

                data->webrtcSignalManager->webrtcSocketMap[accountId] = data->webrtcSignalSocket;

                response["state"] = 200;

                response["message"] = "Register Successful";

                response["accountId"] = accountId;

                data->webrtcSignalSocket->asyncWrite(boost::json::serialize(response));

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

        }

        void WebRTCLogicSystem::initHttpHandlers()
        {

            std::function<void(std::shared_ptr<HttpSocket>, unsigned, boost::json::object)> httpSocketAsyncWrite = [this](std::shared_ptr<HttpSocket> httpSocket, unsigned version, boost::json::object body) {

                boost::asio::io_context& ioContext = httpSocket->getIoContext();

                boost::asio::co_spawn(ioContext, [httpSocket = std::move(httpSocket), version, body = std::move(body)]()mutable->boost::asio::awaitable<void> {

                    boost::beast::http::response<boost::beast::http::string_body> res{ boost::beast::http::status::ok, version };

                    res.set(boost::beast::http::field::content_type, "application/json");

                    res.body() = boost::json::serialize(body);

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

                boost::json::object resp;

                resp["state"] = code;

                resp["message"] = msg;

                resp["data"] = nullptr;

                httpSocketAsyncWrite(httpSocket, version, std::move(resp));

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

                boost::json::object data;

                data["totalManagers"] = server->getChannelNumbers();

                boost::json::object resp;

                resp["state"] = 200;

                resp["message"] = "success";

                resp["data"] = std::move(data);

                httpSocketAsyncWrite(httpSocket, httpRequest.version(), std::move(resp));

                co_return;

                };


            httpHandlers["/api/v1/managers/stat"] = [this, httpSocketAsyncWrite, httpSocketAsyncWriteError, verifyAuthorization](std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest) mutable -> boost::asio::awaitable<void> {

                if (!verifyAuthorization(httpRequest)) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 403, "Forbidden");

                    co_return;

                }

                boost::json::value reqBody = boost::json::parse(httpRequest.body());

                if (!reqBody.is_object() || !reqBody.as_object().contains("channelIndex")) {

                    httpSocketAsyncWriteError(httpSocket, httpRequest.version(), 400, "Missing channelIndex");

                    co_return;

                }

                size_t targetIdx = reqBody.as_object()["channelIndex"].as_int64();

                int currentChannelIndex = httpSocket->getWebRTCSignalManager()->channelIndex;

                WebRTCSignalServer* server = httpSocket->getWebRTCSignalManager()->webrtcSignalServer;

                server->postTaskAsync(targetIdx, [this, httpSocket = httpSocket->shared_from_this(), version = httpRequest.version(), currentChannelIndex, httpSocketAsyncWrite](std::shared_ptr<WebRTCSignalManager> targetManager) mutable -> boost::asio::awaitable<void> {

                    boost::json::object targetData;

                    targetData["channelIndex"] = targetManager->channelIndex;

                    targetData["totalSockets"] = targetManager->webrtcSocketMap.size();

                    boost::json::array socketList;

                    for (auto const& [accountId, socketPtr] : targetManager->webrtcSocketMap) {

                        boost::json::object sInfo;

                        sInfo["accountId"] = accountId;

                        sInfo["remoteAddr"] = socketPtr->getRemoteAddress();

                        sInfo["sessionId"] = socketPtr->getSessionId();

                        sInfo["isRegistered"] = socketPtr->getRegistered();

                        sInfo["cachedRouteCount"] = socketPtr->actorMappingIndex.size();

                        socketList.push_back(std::move(sInfo));

                    }

                    targetData["sockets"] = std::move(socketList);

                    targetManager->webrtcSignalServer->postTaskAsync(currentChannelIndex, [this, httpSocket = httpSocket->shared_from_this(), version, targetData = std::move(targetData), httpSocketAsyncWrite](std::shared_ptr<WebRTCSignalManager> manager) mutable -> boost::asio::awaitable<void> {

                        boost::json::object resp;

                        resp["state"] = 200;

                        resp["data"] = std::move(targetData);

                        httpSocketAsyncWrite(httpSocket, version, std::move(resp));

                        co_return;

                        });

                    co_return;

                    });

                co_return;

                };

        }
    }

}