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

                LOG_ERROR("Unknow WebRTC Request Type: %d", type);

            }

        }


        void WebRTCLogicSystem::initHandlers() {

            auto self = shared_from_this();

            std::function<boost::asio::awaitable <void>(std::shared_ptr<WebRTCSignalData>, std::string)> forwardHandler = [self](std::shared_ptr<WebRTCSignalData> data, std::string requestTypeStr)->boost::asio::awaitable<void> {

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

                                        LOG_WARNING("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

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

                                LOG_WARNING("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

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

                                                LOG_WARNING("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

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

                                        LOG_WARNING("Request forward: %s -> %s (Request Type: %s)", accountID.c_str(), targetID.c_str(), requestTypeStr);

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

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REGISTER)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                boost::json::object& message = data->json;

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                if (!message.contains("accountID")) {
                    LOG_WARNING("REGISTER Message Missing accountID.");
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

                data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = data->webrtcSignalManager->shared_from_this(), accountID, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                    manager->getActorSocketMappingIndex()[accountID] = self->channelIndex;

                    co_return;

                    });


                LOG_INFO("User Register Successful : %s (channelIndex: %d)", accountID.c_str(), data->webrtcSignalManager->channelIndex);

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::REQUEST)] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), "REQUEST");

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::RESTART)] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), "RESTART");

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::STOPREMOTE)] = [self, forwardHandler](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                co_await forwardHandler(std::move(data), "STOPREMOTE");

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOSE)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                std::string accountID = data->webrtcSignalSocket->getAccountID();

                if (!accountID.empty()) {

                    data->webrtcSignalManager->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
                }
                data->webrtcSignalSocket->stop(); // 关闭 socket 实例

                LOG_INFO("Receive User %s CLOSE Request，WebRTCSignalSocket is Stop", accountID.c_str());


                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOUD_GAME_SERVERS_REGISTER)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                boost::json::object response;

                response["requestType"] = static_cast<int64_t>(WebRTCRequestState::CLOUD_PROCESS_LOGIN);

                auto webrtcSignalSocket = data->webrtcSignalSocket;

                auto json = data->json;

                auto manager = data->webrtcSignalManager;

                auto conn = manager->webrtcMysqlManager->getConnection();

                std::string serverIP = webrtcSignalSocket->getSocket()
                    .remote_endpoint()
                    .address()
                    .to_string();

                boost::mysql::results selectResult;

                boost::mysql::statement stmt = co_await conn->async_prepare_statement("select * from game_servers where ip_address = ?", boost::asio::use_awaitable);

                co_await conn->async_execute(stmt.bind(serverIP), selectResult);

                if (!selectResult.rows().empty()) {

                    response["state"] = 500;

                    response["message"] = "This server ip already be used";

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    co_return;

                }

                // 3. 解析请求字段
                if (!json.contains("maxProcess") || !json.contains("name") || !json.contains("hostname") ||
                    !json.contains("location") || !json.contains("region")) {
                    response["state"] = 400;
                    response["message"] = "缺少必要字段";
                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));
                    co_return;
                }

                boost::uuids::random_generator gen;
                std::string serverId = boost::uuids::to_string(gen());

                int max_process = json["maxProcess"].as_int64();
                std::string name = json["name"].as_string().c_str();
                std::string hostname = json["hostname"].as_string().c_str();
                std::string location = json["location"].as_string().c_str();
                std::string region = json["region"].as_string().c_str();
                std::string tags = json.contains("tags") ? boost::json::serialize(json["tags"]) : "{}";
                std::string specifications = json.contains("specifications") ? boost::json::serialize(json["specifications"]) : "{}";

                // 4. 插入数据库
                boost::mysql::statement insertStmt = co_await conn->async_prepare_statement(
                    R"(INSERT INTO game_servers (server_id, ip_address, name, hostname, location, specifications, max_processes, 
            current_processes, status, region, tags, last_heartbeat, created_at, updated_at, del_flag) VALUES (?, ?, ?, ?, ?, ?, ?, 0, 'online', ?, ?, NOW(), NOW(), NOW(), 0))",
                    boost::asio::use_awaitable
                );

                boost::mysql::results insertResult;
                co_await conn->async_execute(
                    insertStmt.bind(
                        serverId, serverIP, name, hostname,
                        location, specifications, max_process,
                        region, tags
                    ), insertResult,
                    boost::asio::use_awaitable
                );


                // 6. 注册成功响应
                response["state"] = 200;
                response["message"] = "注册成功";
                response["serverId"] = serverId;
                response["maxProcess"] = max_process;

                webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                manager->webrtcSignalSocketMap[serverId] = webrtcSignalSocket;
                // 7. 设置 socket 状态
                webrtcSignalSocket->setRegistered(true);

                webrtcSignalSocket->setAccountID(serverId);  // 可自定义

                LOG_INFO("CloudGame is Register Successful: ID=%llu, IP=%s, Name=%s",
                    serverId, serverIP.c_str(), name.c_str());

                co_return;

                };

            webrtcHandlers[static_cast<int>(WebRTCRequestState::CLOUD_PROCESS_LOGIN)] =
                [self](std::shared_ptr<WebRTCSignalData> data) -> boost::asio::awaitable<void> {
                // ------------------- 基础准备 -------------------
                LOG_INFO("Process CLOUD_PROCESS_LOGIN Request");

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
                auto sendError = [&](int code, const std::string& msg) {
                    response["state"] = code;
                    response["message"] = msg;
                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));
                    };

                auto registerSocket = [&](const std::string& processId) {

                    webrtcSignalSocket->setRegistered(true);

                    webrtcSignalSocket->setAccountID(processId);

                    webrtcSignalSocket->setCloudProcess(true);

                    manager->webrtcSignalSocketMap[processId] = webrtcSignalSocket;

                    int mapChannelIndex = data->webrtcSignalManager->hasher(processId) % data->webrtcSignalManager->hashSize;

                    data->webrtcSignalManager->webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = data->webrtcSignalManager->shared_from_this(), processId, mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager)->boost::asio::awaitable<void> {

                        manager->getActorSocketMappingIndex()[processId] = self->channelIndex;

                        co_return;

                        });


                    };

                auto updateCurrentProcesses = [&](int64_t cur) -> boost::asio::awaitable<bool> {
                    boost::mysql::statement stmt = co_await conn->async_prepare_statement(
                        "UPDATE game_servers SET current_processes = ? WHERE server_id = ?", boost::asio::use_awaitable);
                    boost::mysql::results r;
                    co_await conn->async_execute(stmt.bind(cur, serverId), r, boost::asio::use_awaitable);
                    co_return r.affected_rows() == 1;
                    };

                // ------------------- 开始事务 -------------------
                boost::mysql::results dummy;
                co_await conn->async_execute("START TRANSACTION", dummy, boost::asio::use_awaitable);

                try {
                    // ---------- 1. 验证服务器 ----------
                    boost::mysql::statement stmt = co_await conn->async_prepare_statement(
                        "SELECT * FROM game_servers WHERE server_id = ? AND ip_address = ?", boost::asio::use_awaitable);
                    boost::mysql::results result;
                    co_await conn->async_execute(stmt.bind(serverId, serverIP), result,
                        boost::asio::use_awaitable);

                    if (result.rows().empty()) {
                        sendError(404, "server not register");
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

                        stmt = co_await conn->async_prepare_statement(
                            "INSERT INTO game_processes "
                            "(process_id, server_id, process_name, game_type, game_version, "
                            "is_idle, startup_parameters, health_status, "
                            "last_health_check, last_heartbeat, started_at, "
                            "created_at, updated_at, del_flag, is_login) "
                            "VALUES (?, ?, ?, ?, ?, 1, '', 'healthy', NULL, NULL, NOW(), "
                            "        NOW(), NOW(), 0, 0)", boost::asio::use_awaitable);

                        boost::mysql::results ins;
                        co_await conn->async_execute(
                            stmt.bind(assignedProcessId, serverId, processName, gameType, gameVersion),
                            ins, boost::asio::use_awaitable);

                        if (ins.affected_rows() != 1) {
                            throw std::runtime_error("GameProcess Insert Error");
                        }

                        if (!co_await updateCurrentProcesses(gameServer.current_processes + 1)) {
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
                            stmt = co_await conn->async_prepare_statement(
                                "UPDATE game_processes SET is_login = 1, is_idle = 1, last_heartbeat = NOW() "
                                "WHERE process_id = ?", boost::asio::use_awaitable);
                            co_await conn->async_execute(stmt.bind(assignedProcessId), result,
                                boost::asio::use_awaitable);
                            LOG_INFO("ReUse The ProcessID: %s", assignedProcessId.c_str());
                        }
                        else if (static_cast<int64_t>(allProcesses.size()) < gameServer.max_processes) {
                            // 新建进程（直接标记为已登录、非空闲）
                            boost::uuids::random_generator gen;
                            assignedProcessId = boost::uuids::to_string(gen());

                            stmt = co_await conn->async_prepare_statement(
                                "INSERT INTO game_processes "
                                "(process_id, server_id, process_name, game_type, game_version, "
                                "is_idle, startup_parameters, health_status, "
                                "last_health_check, last_heartbeat, started_at, "
                                "created_at, updated_at, del_flag, is_login) "
                                "VALUES (?, ?, ?, ?, ?, 0, '', 'healthy', NULL, NULL, NOW(), "
                                "        NOW(), NOW(), 0, 1)", boost::asio::use_awaitable);

                            boost::mysql::results ins;
                            co_await conn->async_execute(
                                stmt.bind(assignedProcessId, serverId, processName, gameType, gameVersion),
                                ins, boost::asio::use_awaitable);

                            if (ins.affected_rows() != 1) {
                                throw std::runtime_error("GameProcess Insert Error");
                            }

                            if (!co_await updateCurrentProcesses(gameServer.current_processes + 1)) {
                                throw std::runtime_error("GameServers Update CurrentProcess Error");
                            }

                            LOG_INFO("New Create Process（Already Login）: %s", assignedProcessId.c_str());
                        }
                        else {
                            // 超限
                            sendError(507, "The Server is Max Process,Can't Create More Process");
                            co_await conn->async_execute("ROLLBACK", dummy, boost::asio::use_awaitable);
                            co_return;
                        }
                    }

                    // ---------- 4. 注册 Socket ----------
                    registerSocket(assignedProcessId);

                    // ---------- 5. 成功返回 ----------
                    response["state"] = 200;
                    response["message"] = "Process Allocate Successful";
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
                    LOG_ERROR("CLOUD_PROCESS_LOGIN Matters: %s", e.what());
                    sendError(500, e.what());
                }

                co_return;
                };


            webrtcHandlers[static_cast<int>(WebRTCRequestState::USER_GET_GAMES_PROCESS_ID)] = [self](std::shared_ptr<WebRTCSignalData> data)->boost::asio::awaitable<void> {

                std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = data->webrtcSignalSocket;

                WebRTCSignalManager * manager = data->webrtcSignalManager;

                std::shared_ptr<boost::mysql::any_connection> connection = manager->webrtcMysqlManager->getConnection();

                boost::json::object json = data->json;

                boost::json::object response;

                response["requestType"] = json["requestType"].as_int64();

                std::string gameType = json["gameType"].as_string().c_str();

                boost::mysql::results selectResult;

                boost::mysql::statement state = co_await connection->async_prepare_statement("SELECT s.server_id,COUNT(p.server_id) AS idle_count FROM game_servers s LEFT JOIN game_processes p ON s.server_id = p.server_id AND p.is_login = 1 AND p.is_idle = 1 WHERE JSON_CONTAINS(s.tags, JSON_OBJECT('tags', ?)) and s.del_flag = 0 and p.del_flag = 0 GROUP BY s.server_id ORDER BY idle_count DESC LIMIT 1",boost::asio::use_awaitable
                );

                co_await connection->async_execute(state.bind(gameType), selectResult, boost::asio::use_awaitable);

                if (selectResult.rows().empty()) {

                    response["state"] = 500;
                
                    response["message"] = "The Cloud Game Type:" + gameType + "Service is not Exist";

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    co_return;
                }

                if (selectResult.rows().at(0).at(1).as_int64() == 0) {
                
                    response["state"] = 500;

                    response["message"] = "The Request Cloud Game Type:" + gameType + "Service is full";

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    co_return;

                }

                std::string serverID = selectResult.rows().at(0).at(0).as_string();

                state = co_await connection->async_prepare_statement("SELECT *  FROM game_processes WHERE del_flag = 0 and game_type = ? and is_login = 1 and is_idle = 1 and server_id = ?", boost::asio::use_awaitable);

                co_await connection->async_execute(state.bind(gameType,serverID),selectResult, boost::asio::use_awaitable);

                if (selectResult.empty()) {
                
                    response["state"] = 500;

                    response["message"] = "The Server:" + serverID + " Type:"+ gameType +" Service is full";

                    webrtcSignalSocket->writerAsync(boost::json::serialize(response));

                    co_return;

                }

                hope::entity::GameProcesses gameProcess(selectResult.rows().at(0));

                response["state"] = 200;

                response["message"] = "Cloud Games Process Allocate Successful";

                response["processId"] = gameProcess.process_id;

                response["serverId"] = gameProcess.server_id;

                webrtcSignalSocket->writerAsync(boost::json::serialize(response));

            };

        }


    }

}



