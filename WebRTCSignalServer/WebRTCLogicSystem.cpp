#include "WebRTCLogicSystem.h"
#include "WebRTCSignalData.h"
#include "WebRTCSignalManager.h"

#include <iostream>
#include <chrono>

#include <boost/uuid/uuid.hpp>            // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>   

#include "GameServers.h"
#include "GameProcesses.h"
#include "Utils.h"



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

        void WebRTCLogicSystem::postMessageToQueue(std::shared_ptr<WebRTCSignalData> data) {

            int type = data->json["requestType"].as_int64();

            if (this->webrtcHandlers.find(type) != this->webrtcHandlers.end()) {

                boost::asio::co_spawn(ioContext, this->webrtcHandlers[type](std::move(data)),
                    [this](std::exception_ptr ptr) {
                        // 正确的异常处理方式
                        if (ptr) { // 重要：检查是否确实有异常发生
                            try {
                                std::rethrow_exception(ptr); // 重新抛出异常
                            }
                            catch (const std::exception& e) {
                                // 现在可以正常捕获并处理了
                                LOG_ERROR("webrtcLogicSystem boost::asio::co_spawn Exception: %s", e.what());
                            }
                        }
                    }
                );

            }
            else {

                LOG_ERROR("未知的 WebRTC 请求类型: %d", type);

            }

        }


        void WebRTCLogicSystem::initHandlers() {

            auto self = shared_from_this();

            std::function<boost::asio::awaitable <void>(std::shared_ptr<WebRTCSignalData>, std::string)> forawrdHandler = [self](std::shared_ptr<WebRTCSignalData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {
                
                boost::json::object message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                int64_t requestTypeValue = message["requestType"].as_int64();

                if (!message.contains("accountID") || !message.contains("targetID")) {
                    LOG_WARNING("转发消息缺少 accountID 或 targetID.");
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

                                        LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), requestTypeStr);

                                    }

                                    });

                            }
                            else {

                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "targetID is not register";
                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), requestTypeStr);

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

                                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

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

                                                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                                            }
                                            else {

                                                boost::json::object response;
                                                response["requestType"] = requestTypeValue;
                                                response["state"] = 404;
                                                response["message"] = "targetID is not register";
                                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                                LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), requestTypeStr);

                                            }

                                            });

                                    }
                                    else {

                                        boost::json::object response;
                                        response["requestType"] = requestTypeValue;
                                        response["state"] = 404;
                                        response["message"] = "targetID is not register";
                                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                        LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)", targetID.c_str(), accountID.c_str(), requestTypeStr);

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

                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REGISTER)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                if (!message.contains("accountID")) {
                    LOG_WARNING("REGISTER 消息缺少 accountID.");
                    co_return;
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

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REQUEST)] = [self, forawrdHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forawrdHandler(std::move(data),"REQUEST");
               
                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::RESTART)] = [self, forawrdHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forawrdHandler(std::move(data), "RESTART");

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::STOPREMOTE)] = [self, forawrdHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forawrdHandler(std::move(data), "STOPREMOTE");

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOSE)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                std::string accountID = data->webrtcSignalSocket->getAccountID();

                if (!accountID.empty()) {

                    data->webrtcSignalManager->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
                }
                data->webrtcSignalSocket->stop(); // 关闭 socket 实例

                LOG_INFO("收到用户 %s 的 CLOSE 请求，连接已停止", accountID.c_str());


                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOUD_PROCESS_LOGIN)] =
                [self](std::shared_ptr<WebRTCSignalData> data) -> boost::asio::awaitable<void> {
                // ------------------- 基础准备 -------------------
                LOG_INFO("处理 CLOUD_PROCESS_LOGIN 请求");

                boost::json::object response;
                response["requestType"] = static_cast<int64_t>(WebRTCRequestState::CLOUD_PROCESS_LOGIN);

                auto webrtcSignalSocket = data->webrtcSignalSocket;
                auto json = data->json;
                auto manager = data->webrtcSignalManager;
                auto conn = manager->webrtcMysqlManager->getConnection();

                // JSON 字段
                std::string serverId = json["serverId"].as_string().c_str();
                std::string processName = json["processName"].as_string().c_str();
                std::string gameType = json["gameType"].as_string().c_str();
                std::string gameVersion = json["gameVersion"].as_string().c_str();

                // 远程 IP
                std::string serverIP = webrtcSignalSocket->getSocket()
                    .remote_endpoint()
                    .address()
                    .to_string();

                // ------------------- 辅助函数 -------------------
                auto send_error = [&](int code, const std::string& msg) {
                    response["state"] = code;
                    response["message"] = msg;
                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));
                    };

                auto register_socket = [&](const std::string& processId) {
                    webrtcSignalSocket->setRegistered(true);
                    webrtcSignalSocket->setAccountID(processId);
					webrtcSignalSocket->setCloudGame(true);
                    manager->webrtcSignalSocketMap[processId] = webrtcSignalSocket;
                    };

                auto update_current_processes = [&](int64_t cur) -> boost::asio::awaitable<bool> {
                    boost::mysql::statement stmt = conn->prepare_statement(
                        "UPDATE game_servers SET current_processes = ? WHERE server_id = ?");
                    boost::mysql::results r;
                    co_await conn->async_execute(stmt.bind(cur, serverId), r, boost::asio::use_awaitable);
                    co_return r.affected_rows() == 1;
                    };

                // ------------------- 开始事务 -------------------
                boost::mysql::results dummy;
                co_await conn->async_execute("START TRANSACTION", dummy, boost::asio::use_awaitable);

                try {
                    // ---------- 1. 验证服务器 ----------
                    boost::mysql::statement stmt = conn->prepare_statement(
                        "SELECT * FROM game_servers WHERE server_id = ? AND ip_address = ?");
                    boost::mysql::results result;
                    co_await conn->async_execute(stmt.bind(serverId, serverIP), result,
                        boost::asio::use_awaitable);

                    if (result.rows().empty()) {
                        send_error(404, "server not register");
                        co_await conn->async_execute("ROLLBACK", dummy, boost::asio::use_awaitable);
                        co_return;
                    }
                    hope::entity::GameServers gameServer(result.rows()[0]);

                    // ---------- 2. 读取已有进程 ----------
                    stmt = conn->prepare_statement("SELECT * FROM game_processes WHERE server_id = ?");
                    co_await conn->async_execute(stmt.bind(serverId), result, boost::asio::use_awaitable);

                    std::vector<hope::entity::GameProcesses> allProcesses;
                    for (auto&& row : result.rows()) {
                        allProcesses.emplace_back(row);
                    }

                    // ---------- 3. 业务分支 ----------
                    std::string assignedProcessId;

                    // 情况 A：首次登录且 max_processes > 0（旧代码分支）
                    if (allProcesses.empty() && gameServer.max_processes > 0) {
                        boost::uuids::random_generator gen;
                        assignedProcessId = boost::uuids::to_string(gen());

                        stmt = conn->prepare_statement(
                            "INSERT INTO game_processes "
                            "(process_id, server_id, process_name, game_type, game_version, "
                            "is_idle, startup_parameters, health_status, "
                            "last_health_check, last_heartbeat, started_at, "
                            "created_at, updated_at, del_flag, is_login) "
                            "VALUES (?, ?, ?, ?, ?, 1, '', 'healthy', NULL, NULL, NOW(), "
                            "        NOW(), NOW(), 0, 0)");

                        boost::mysql::results ins;
                        co_await conn->async_execute(
                            stmt.bind(assignedProcessId, serverId, processName, gameType, gameVersion),
                            ins, boost::asio::use_awaitable);

                        if (ins.affected_rows() != 1) {
                            throw std::runtime_error("GameProcess Insert Error");
                        }

                        if (!co_await update_current_processes(gameServer.current_processes + 1)) {
                            throw std::runtime_error("GameServers Update CurrentProcess Error");
                        }

                        LOG_INFO("首次登录创建进程: %s", assignedProcessId.c_str());
                    }
                    // 情况 B：已有进程，尝试复用/新建
                    else {
                        // 筛选空闲、可登录的进程
                        std::vector<hope::entity::GameProcesses> idle;
                        for (const auto& p : allProcesses) {
                            if (p.game_type == gameType && p.is_login == 0 && p.is_idle == 1 &&
                                p.del_flag == 0 && p.health_status == "healthy") {
                                idle.push_back(p);
                            }
                        }

                        if (!idle.empty()) {
                            // 复用第一个空闲进程
                            assignedProcessId = idle[0].process_id;
                            stmt = conn->prepare_statement(
                                "UPDATE game_processes SET is_login = 1, is_idle = 0, last_heartbeat = NOW() "
                                "WHERE process_id = ?");
                            co_await conn->async_execute(stmt.bind(assignedProcessId), result,
                                boost::asio::use_awaitable);
                            LOG_INFO("复用空闲进程: %s", assignedProcessId.c_str());
                        }
                        else if (static_cast<int64_t>(allProcesses.size()) < gameServer.max_processes) {
                            // 新建进程（直接标记为已登录、非空闲）
                            boost::uuids::random_generator gen;
                            assignedProcessId = boost::uuids::to_string(gen());

                            stmt = conn->prepare_statement(
                                "INSERT INTO game_processes "
                                "(process_id, server_id, process_name, game_type, game_version, "
                                "is_idle, startup_parameters, health_status, "
                                "last_health_check, last_heartbeat, started_at, "
                                "created_at, updated_at, del_flag, is_login) "
                                "VALUES (?, ?, ?, ?, ?, 0, '', 'healthy', NULL, NULL, NOW(), "
                                "        NOW(), NOW(), 0, 1)");

                            boost::mysql::results ins;
                            co_await conn->async_execute(
                                stmt.bind(assignedProcessId, serverId, processName, gameType, gameVersion),
                                ins, boost::asio::use_awaitable);

                            if (ins.affected_rows() != 1) {
                                throw std::runtime_error("GameProcess Insert Error");
                            }

                            if (!co_await update_current_processes(gameServer.current_processes + 1)) {
                                throw std::runtime_error("GameServers Update CurrentProcess Error");
                            }

                            LOG_INFO("新建进程（已登录）: %s", assignedProcessId.c_str());
                        }
                        else {
                            // 超限
                            send_error(507, "服务器已达到最大进程数限制，无法创建新进程");
                            co_await conn->async_execute("ROLLBACK", dummy, boost::asio::use_awaitable);
                            co_return;
                        }
                    }

                    // ---------- 4. 注册 Socket ----------
                    register_socket(assignedProcessId);

                    // ---------- 5. 成功返回 ----------
                    response["state"] = 200;
                    response["message"] = "进程分配成功";
                    response["processId"] = assignedProcessId;
                    response["processName"] = processName;
                    response["gameType"] = gameType;

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    // 提交事务
                    co_await conn->async_execute("COMMIT", dummy, boost::asio::use_awaitable);
                }
                catch (const std::exception& e) {
                    // 任何异常都回滚
                    conn->async_execute("ROLLBACK", dummy, boost::asio::detached);
                    LOG_ERROR("CLOUD_PROCESS_LOGIN 事务失败: %s", e.what());
                    send_error(500, e.what());
                }

                co_return;
                };


        }


	}

}



