#include "WebRTCSignalManager.h"
#include "WebRTCSignalServer.h"

namespace Hope {
	
	WebRTCSignalManager::WebRTCSignalManager(boost::asio::io_context & ioContext,int channelIndex,WebRTCSignalServer* webrtcSignalServer): ioContext(ioContext)
		, channelIndex(channelIndex)
        , webrtcSignalServer(webrtcSignalServer)
	{
	}

	WebRTCSignalManager::~WebRTCSignalManager()
	{
		webrtcSignalSocketMap.clear();
	}

	std::shared_ptr<WebRTCSignalSocket> WebRTCSignalManager::generateWebRTCSignalSocket()
	{

        std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<WebRTCSignalSocket>(ioContext, channelIndex, this);

        webrtcSignalSocket->setOnDisConnectHandle([self = shared_from_this()](std::string accountID) {
            self->removeConnection(accountID); // 清理连接映射
            });

		return webrtcSignalSocket;
	}

	boost::asio::io_context& WebRTCSignalManager::getIoComplatePorts()
	{
		return ioContext;
	}

	void WebRTCSignalManager::handleMessage(boost::json::object message, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket)
	{
        LOG_DEBUG("收到消息: %s", boost::json::serialize(message).c_str());

        // 检查 requestType 字段是否存在
        if (!message.contains("requestType")) {
            // [可选] 发送错误响应给客户端
            LOG_WARNING("收到缺少 requestType 字段的无效消息.");
            return;
        }

        // 安全地提取 requestType 并转换为枚举
        int64_t requestTypeValue = message["requestType"].as_int64();
        
        WebRTCRequestState requestType = WebRTCRequestState(requestTypeValue);

        switch (requestType) {

        case WebRTCRequestState::REGISTER: {
            // --------------------------------------------------
            // A. 注册逻辑
            // --------------------------------------------------
            if (!message.contains("accountID")) {
                LOG_WARNING("REGISTER 消息缺少 accountID.");
                break;
            }
            std::string accountID = message["accountID"].as_string().c_str();

            webrtcSignalSocket->setAccountID(accountID); // 假设 setAccountID 存在
            webrtcSignalSocket->setRegistered(true);

            webrtcSignalSocketMap[accountID] = webrtcSignalSocket;

            boost::json::object response;
            response["requestType"] = static_cast<int64_t>(WebRTCRequestState::REGISTER);
            response["state"] = 200;
            response["message"] = "register successful";

            webrtcSignalSocket->writerAsync(boost::json::serialize(response));

            int mapChannelIndex = hasher(accountID) % hashSize;

            webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = shared_from_this(), accountID,mapChannelIndex](std::shared_ptr<WebRTCSignalManager> manager) {
                
				manager->getActorSocketMappingIndex()[accountID] = self->channelIndex;

                });
            

            LOG_INFO("用户注册成功: %s (channelIndex: %d)", accountID.c_str(),channelIndex);
            break;
        }

        case WebRTCRequestState::REQUEST:
        case WebRTCRequestState::RESTART:
        case WebRTCRequestState::STOPREMOTE: {

            if (!message.contains("accountID") || !message.contains("targetID")) {
                LOG_WARNING("转发消息缺少 accountID 或 targetID.");
                break;
            }

            std::string accountID = message["accountID"].as_string().c_str(); // 发送方 ID
            std::string targetID = message["targetID"].as_string().c_str();   // 目标方 ID

            std::shared_ptr<WebRTCSignalSocket> targetSocket = nullptr;

            // 1. 查找目标连接 (使用哈希锁)
            {
                auto it = webrtcSignalSocketMap.find(targetID);
                if (it != webrtcSignalSocketMap.end()) {
                    targetSocket = it->second;
                }
            }

            // 2. 处理目标未找到 (404)
            if (!targetSocket) {

                int mapChannelIndex = hasher(targetID) % hashSize;

                auto self = shared_from_this();

                webrtcSignalServer->postAsyncTask(mapChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {

                    if (manager->getActorSocketMappingIndex().find(targetID) != manager->getActorSocketMappingIndex().end()) {
                        
						int targetChannelIndex = manager->getActorSocketMappingIndex()[targetID];

                        self->webrtcSignalServer->postAsyncTask(targetChannelIndex, [=](std::shared_ptr<WebRTCSignalManager> manager) {
                            
                            if (manager->webrtcSignalSocketMap.find(targetID) != manager->webrtcSignalSocketMap.end()) {
                                
                                boost::json::object forwardMessage = message; // 复制原始消息体
                                forwardMessage["state"] = 200;
                                forwardMessage["message"] = "WebRTCSignalServer forward";

                                manager->webrtcSignalSocketMap[targetID]->writerAsync(boost::json::serialize(forwardMessage));

                                LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",
                                    accountID.c_str(), targetID.c_str(),
                                    (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));

                            }
                            else {
                            
                                boost::json::object response;
                                response["requestType"] = requestTypeValue;
                                response["state"] = 404;
                                response["message"] = "targetID is not register";
                                webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                                LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",
                                    targetID.c_str(), accountID.c_str(),
                                    (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));
                                
                            }

                            });

                    }
                    else {

                        boost::json::object response;
                        response["requestType"] = requestTypeValue;
                        response["state"] = 404;
                        response["message"] = "targetID is not register";
                        webrtcSignalSocket->writerAsync(boost::json::serialize(response)); // 响应发送方

                        LOG_WARNING("目标用户未找到: %s (来自: %s, 请求类型: %s)",
                            targetID.c_str(), accountID.c_str(),
                            (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));

                    }

                    });

                return;
            }

            // 3. 转发消息
            boost::json::object forwardMessage = message; // 复制原始消息体
            forwardMessage["state"] = 200;
            forwardMessage["message"] = "WebRTCSignalServer forward";
            targetSocket->writerAsync(boost::json::serialize(forwardMessage)); // 转发给目标方

            LOG_INFO("消息转发成功: %s -> %s (请求类型: %s)",
                accountID.c_str(), targetID.c_str(),
                (requestType == WebRTCRequestState::REQUEST ? "REQUEST" : (requestType == WebRTCRequestState::RESTART ? "RESTART" : "STOPREMOTE")));
            break;
        }

        case WebRTCRequestState::CLOSE: {
            std::string accountID = webrtcSignalSocket->getAccountID();
            if (!accountID.empty()) {
                this->removeConnection(accountID); // 假设 removeConnection 封装了哈希桶移除逻辑
            }
            webrtcSignalSocket->stop(); // 关闭 socket 实例
            LOG_INFO("收到用户 %s 的 CLOSE 请求，连接已停止", accountID.c_str());
            break;
        }

        default: {
            LOG_WARNING("收到未知的请求类型: %lld", requestTypeValue);
            break;
        }
        }
	}

    void WebRTCSignalManager::removeConnection(const std::string& accountID)
    {
		LOG_INFO("移除连接: %s", accountID.c_str());

        webrtcSignalSocketMap.erase(accountID);

        int mapChannelIndex = hasher(accountID) % hashSize;

        LOG_INFO("开始异步回调: %d", mapChannelIndex);

        webrtcSignalServer->postAsyncTask(mapChannelIndex, [self = shared_from_this(), accountID](std::shared_ptr<WebRTCSignalManager> manager) {

            manager->getActorSocketMappingIndex().unsafe_erase(accountID);

            });

    }

    tbb::concurrent_unordered_map<std::string, int>& WebRTCSignalManager::getActorSocketMappingIndex()
    {
		return actorSocketMappingIndex;
    }

	

}
