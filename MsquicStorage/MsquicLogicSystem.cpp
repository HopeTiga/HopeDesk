#include "MsquicLogicSystem.h"
#include "MsquicSignalServer.h"
#include "MsquicManager.h"
#include "MsquicSocketInterface.h"
#include "MsquicSocket.h"
#include "WebRTCSignalSocket.h"
#include "MsquicData.h"
#include "MsquicMysqlManagerPools.h"


#include <iostream>
#include <chrono>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include <jwt-cpp/jwt.h>

#include "MsquicHashMap.h"
#include "MsquicHashSet.h"

#include "AsyncTransactionGuard.h"

#include "ConfigManager.h"
#include "GameServers.h"
#include "GameProcesses.h"
#include "PlayerSessions.h"
#include "Utils.h"

hope::utils::MsquicHashMap<std::string, hope::utils::MsquicHashSet<std::string>> cloudProcessHashMap;

namespace hope {

    namespace handle
    {

        MsquicLogicSystem::MsquicLogicSystem(boost::asio::io_context& ioContext) : ioContext(ioContext)
        {


        }

        void MsquicLogicSystem::RunEventLoop() {
           
            initHandlers();

        }

        boost::asio::io_context& MsquicLogicSystem::getIoCompletePorts()
        {
            return ioContext;
        }

        MsquicLogicSystem::~MsquicLogicSystem() {
			msquicHandlers.clear();
        }

        void MsquicLogicSystem::postTaskAsync(std::shared_ptr<hope::quic::MsquicData> data) {
            
            data->json = makeCleanCopy(data->json);

            int type = data->json["requestType"].as_int64();

            if (this->msquicHandlers.find(type) != this->msquicHandlers.end()) {
                std::function<boost::asio::awaitable<void>(std::shared_ptr<hope::quic::MsquicData>)> func = this->msquicHandlers[type];

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
                                LOG_ERROR("MsquicLogicSystem boost::asio::co_spawn Task: %d Exception: %s", type, e.what());
                            }
                        }
                    });

            }
            else {
                LOG_ERROR("Unknown Msquic Request Type: %d", type);
            }
        }

        void MsquicLogicSystem::initHandlers() {

            auto self = shared_from_this();

            std::function<boost::asio::awaitable<void>(std::shared_ptr<hope::quic::MsquicData>, std::string)> forwardHandler = [self](std::shared_ptr<hope::quic::MsquicData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {

                boost::json::object message = data->json;

                auto msquicSocketInterface = data->msquicSocketInterface.get();

                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountId") || !message.contains("targetId")) {
                    LOG_WARNING("Forward Message Missing accountId or targetId.");
                    co_return;
                }

                std::string accountId = message["accountId"].as_string().c_str();
                std::string targetId = message["targetId"].as_string().c_str();
                std::shared_ptr<hope::quic::MsquicSocketInterface> targetSocket = nullptr;

                {
                    auto it = data->msquicManager->msquicSocketMap.find(targetId);
                    if (it != data->msquicManager->msquicSocketMap.end()) {
                        targetSocket = it->second;
                    }
                }

                if (!targetSocket) {

                    tbb::concurrent_lru_cache<std::string, int>::handle handles = data->msquicManager->localRouteCache[targetId];

                    auto senderManager = data->msquicManager->shared_from_this(); // 发起请求的 Manager

                    auto doGlobalLookup = [=](std::shared_ptr<hope::quic::MsquicManager> initiatorManager) {
                        int mapChannelIndex = initiatorManager->hasher(targetId) % initiatorManager->hashSize;

                        initiatorManager->msquicSignalServer->postTaskAsync(mapChannelIndex, [=](std::shared_ptr<hope::quic::MsquicManager> indexManager)->boost::asio::awaitable<void> {
                            // 步骤 A: 在索引 Manager 查找目标所在的 Channel
                            if (indexManager->actorSocketMappingIndex.find(targetId) != indexManager->actorSocketMappingIndex.end()) {
                                int targetChannelIndex = indexManager->actorSocketMappingIndex[targetId];

                                // 步骤 B: 投递到目标 Socket 所在的 Manager
                                data->msquicManager->msquicSignalServer->postTaskAsync(targetChannelIndex, [=](std::shared_ptr<hope::quic::MsquicManager> destManager)->boost::asio::awaitable<void> {
                                    // 步骤 C: 在目标 Manager 查找 Socket
                                    if (destManager->msquicSocketMap.find(targetId) != destManager->msquicSocketMap.end()) {
                                        // C1. 找到了！更新发起者的本地缓存 (LRU)
                                        // 注意：initiatorManager->localRouteCache 是线程安全的 TBB 容器，可以直接更新
                                        if (tbb::concurrent_lru_cache<std::string, int>::handle h = initiatorManager->localRouteCache[targetId]) {
                                            h.value() = destManager->channelIndex;
                                        }

                                        // C2. 发送消息
                                        std::shared_ptr<hope::quic::MsquicSocketInterface> targetMsquicSocketInterface = destManager->msquicSocketMap[targetId];
                                        boost::json::object forwardMessage = message;
                                        forwardMessage["state"] = 200;
                                        forwardMessage["message"] = "MsquicSignalServer forward";

                                        auto [buffer, size] = buildMessage(forwardMessage, targetMsquicSocketInterface.get());
                                        targetMsquicSocketInterface->writeAsync(buffer, size);

                                        LOG_INFO("Request forward (Global): %s -> %s (Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());
                                    }
                                    else {
                                        // C3. 索引指向了这里，但这里真的没有 (索引脏了或断连竞态)
                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "Target socket not found (Stale Index)";

                                        auto [buffer, size] = buildMessage(response, msquicSocketInterface);
                                        msquicSocketInterface->writeAsync(buffer, size);

                                        LOG_WARNING("Request forward 404 (Stale Index): %s -> %s", accountId.c_str(), targetId.c_str());
                                    }
                                    co_return;
                                    });
                            }
                            else {
                                // 索引里也没找到
                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "Target not registered";

                                auto [buffer, size] = buildMessage(response, msquicSocketInterface);
                                msquicSocketInterface->writeAsync(buffer, size);

                                LOG_WARNING("Request forward 404 (No Index): %s -> %s", accountId.c_str(), targetId.c_str());
                            }
                            co_return;
                            });
                        };

                    // 开始判断缓存状态
                    if (handles.value() == -1) {
                        // Case A: 缓存未命中 -> 直接走全局查找
                        doGlobalLookup(senderManager);
                    }
                    else {
                        // Case B: 缓存命中 -> 尝试投递到缓存的 Manager
                        int cachedChannelIndex = handles.value();

                        data->msquicManager->msquicSignalServer->postTaskAsync(cachedChannelIndex, [=](std::shared_ptr<hope::quic::MsquicManager> cachedManager)->boost::asio::awaitable<void> {
                            if (cachedManager->msquicSocketMap.find(targetId) != cachedManager->msquicSocketMap.end()) {
                                // B1: 缓存有效，直接转发
                                // 更新缓存活跃度 (TBB LRU handle 获取即更新)
                                if (tbb::concurrent_lru_cache<std::string, int>::handle h = senderManager->localRouteCache[targetId]) {
                                    h.value() = cachedManager->channelIndex;
                                }

                                std::shared_ptr<hope::quic::MsquicSocketInterface> targetMsquicSocketInterface = cachedManager->msquicSocketMap[targetId];
                                boost::json::object forwardMessage = message;
                                forwardMessage["state"] = 200;
                                forwardMessage["message"] = "MsquicSignalServer forward";

                                auto [buffer, size] = buildMessage(forwardMessage, targetMsquicSocketInterface.get());
                                targetMsquicSocketInterface->writeAsync(buffer, size);

                                LOG_INFO("Request forward (Cache Hit): %s -> %s (Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());
                            }
                            else {
                                // B2: 【关键修复】缓存失效 (Stale Cache) -> 回退到全局查找
                                LOG_WARNING("Cache Stale for %s (Cached: %d). Fallback to Global Lookup.", targetId.c_str(), cachedManager->channelIndex);

                                // 直接调用全局查找逻辑
                                doGlobalLookup(senderManager);
                            }
                            co_return;
                            });
                    }
                    co_return;
                }

                // 3. 本地找到了，直接转发
                boost::json::object forwardMessage = message;
                forwardMessage["state"] = 200;
                forwardMessage["message"] = "MsquicSignalServer forward";

                auto [buffer, size] = buildMessage(forwardMessage, targetSocket.get());
                targetSocket->writeAsync(buffer, size);

                LOG_INFO("Request forward (Local): %s -> %s (Type: %s)", accountId.c_str(), targetId.c_str(), requestTypeStr.c_str());
                };


            // REGISTER handler
            msquicHandlers[0] = [self](std::shared_ptr<hope::quic::MsquicData> data)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                if (data->msquicSocketInterface->getType() != hope::quic::SocketType::WebSocket) co_return;

                auto msquicSocketInterface = static_cast<hope::socket::WebRTCSignalSocket*>(data->msquicSocketInterface.get());

                std::shared_ptr<hope::mysql::MsquicMysqlManager> msquicMysqlManager = co_await hope::mysql::MsquicMysqlManagerPools::getInstance()->getTransactionMysqlManager();

                boost::json::object response;

                response["requestType"] = message["requestType"].as_int64();

                if (!message.contains("authorization")) {

                    LOG_WARNING("REGISTER Message Missing Authorization.");

                    response["state"] = 500;

                    response["message"] = "REGISTER Message Missing Authorization.";

                    auto [buffer, size] = buildMessage(response, msquicSocketInterface);

                    msquicSocketInterface->writeAsync(buffer, size);

                    co_return;
                }

                std::string authorization = message["authorization"].as_string().c_str();

                std::string encodedata;

                std::string userIdentifier;

                std::string accountId;

                try {

                    jwt::decoded_jwt<jwt::traits::kazuho_picojson> jwtAuthorization = jwt::decode(authorization);

                    jwt::verifier<jwt::default_clock, jwt::traits::kazuho_picojson> verifier = jwt::verify().allow_algorithm(jwt::algorithm::rs256{ publicKey.c_str() ,privateKey.c_str() });

                    verifier.verify(jwtAuthorization);

                    if (!jwtAuthorization.has_payload_claim("encodedata")) {

                        response["state"] = 500;

                        response["message"] = "REGISTER Authorization Missing Encodedata.";

                        auto [buffer, size] = buildMessage(response, msquicSocketInterface);

                        msquicSocketInterface->writeAsync(buffer, size);

                        co_return;
                    }

                    encodedata = jwtAuthorization.get_payload_claim("encodedata").as_string();

                    boost::json::object json = boost::json::parse(encodedata).as_object();

                    userIdentifier = std::to_string(json["id"].as_int64());

					accountId = "user_" + userIdentifier;

                }
                catch (const std::exception& e) {

                    LOG_ERROR("Jwt-CPP Decode ERROR: %s", e.what());

                    response["state"] = 501;

                    response["message"] = "Jwt-CPP Decode ERROR: " + std::string(e.what());

                    auto [buffer, size] = buildMessage(response, msquicSocketInterface);

                    msquicSocketInterface->writeAsync(buffer, size);

                    co_return;
                }

               
                int mapChannelIndex = data->msquicManager->hasher(accountId) % data->msquicManager->hashSize;

                data->msquicManager->msquicSignalServer->postTaskAsync(mapChannelIndex, [data = std::move(data), accountId, mapChannelIndex](std::shared_ptr<hope::quic::MsquicManager> manager)->boost::asio::awaitable<void> {

                    auto msquicSocketInterface = static_cast<hope::socket::WebRTCSignalSocket*>(data->msquicSocketInterface.get());

                    boost::json::object response;

                    response["requestType"] = 0;

                    if (manager->actorSocketMappingIndex.find(accountId) == manager->actorSocketMappingIndex.end()) {

                        msquicSocketInterface->setAccountId(accountId);

                        msquicSocketInterface->setRegistered(true);

                        data->msquicManager->msquicSocketMap[accountId] = data->msquicSocketInterface;

                        response["state"] = 200;

                        response["message"] = "Register Successful";

                        response["accountId"] = accountId;

                        auto [buffer, size] = buildMessage(response, msquicSocketInterface);

                        msquicSocketInterface->writeAsync(buffer, size);

                        manager->actorSocketMappingIndex[accountId] = data->msquicManager->channelIndex;

                        LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountId.c_str(), data->msquicManager->channelIndex);

                    }
                    else {

                        response["state"] = 502;

                        response["message"] = "The User Already Login";

                        response["accountId"] = accountId;

                        auto [buffer, size] = buildMessage(response, msquicSocketInterface);

                        msquicSocketInterface->writeAsync(buffer, size);

                    }

                    co_return;

                    });

                };
            // REQUEST handler
            msquicHandlers[1] = [self, forwardHandler](std::shared_ptr<hope::quic::MsquicData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "REQUEST");
                };
            // RESTART handler
            msquicHandlers[2] = [self, forwardHandler](std::shared_ptr<hope::quic::MsquicData> data)->boost::asio::awaitable<void> {
              co_await forwardHandler(std::move(data), "RESTART");
               };
            // STOPREMOTE handler
            msquicHandlers[3] = [self, forwardHandler](std::shared_ptr<hope::quic::MsquicData> data)->boost::asio::awaitable<void> {
                co_await forwardHandler(std::move(data), "STOPREMOTE");
                };

                // CLOSE handler
            msquicHandlers[4] = [self](std::shared_ptr<hope::quic::MsquicData> data)->boost::asio::awaitable<void> {
                co_return;
                };

                
                
        }

    }

}