#pragma once
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <absl/container/node_hash_map.h>

#include "WebRTCLogicSystem.h"
#include "WebRTCSignalSocket.h"
#include "HttpSocket.h"

#include "AwaitableTask.h"

namespace hope {

	namespace signal {

		class WebRTCSignalServer;

		// 单个信号通道(一个 WebRTCSignalManager)在运行期需要的标量配置。
		// 仅标量,刻意不依赖 WebRTCSignalConfig/CoroRpc 头,避免把 RPC 头拖进 Manager。
		// 由 WebRTCSignalServer::initialize() 从 WebRTCSignalConfig 拆出来注入。
		struct WebRTCSignalChannelConfig {

			size_t hashSize = 1;          // actor 路由分桶数(= 通道数 threadSize)

			int threshold = 256;          // 全局任务队列高水位

			int exitThreshold = 128;      // 本地队列高水位,触发切本地处理

			int asyncThreshold = 32;      // 异步派发阈值

			int socketWaitTime = 10000; // WebSocket 握手超时(ms)

		};

		class WebRTCSignalManager : public std::enable_shared_from_this<WebRTCSignalManager>
		{
			friend class hope::signal::WebRTCLogicSystem;
		public:

			WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer* webrtcSignalServer, TaskChannel& taskQueues, WebRTCSignalChannelConfig channelConfig);

			~WebRTCSignalManager();

			std::shared_ptr<hope::signal::WebRTCLogicSystem> getLogicSystem();

			void removeConnection(std::string accountId, std::string sessionId);

			int getChannelIndex();

			boost::asio::io_context& getIoCompletionPorts();

			std::shared_ptr<hope::signal::WebRTCSignalSocket> generateWebRTCSignalSocket();

			std::shared_ptr<HttpSocket> generateHttpSocket();

			void registerSocket(const std::string& accountId, std::shared_ptr<WebRTCSignalSocket> socket);

#ifdef __linux__

			void asyncAccept(std::atomic<bool>& runAccepct, boost::asio::ip::tcp::endpoint endpoint, boost::asio::ip::tcp::endpoint httpEndpoint, int enableHttp = 0);

#endif

		public:

			struct ActorMapping
			{

				std::string sessionId;

				int channelIndex;

			};

		private:

			boost::asio::io_context& ioContext;

			WebRTCSignalServer* webrtcSignalServer;

			size_t channelIndex;

			std::shared_ptr<hope::signal::WebRTCLogicSystem> logicSystem;

			absl::node_hash_map<std::string, std::shared_ptr<WebRTCSignalSocket>> webrtcSocketMap;

			size_t hashSize;

			WebRTCSignalChannelConfig channelConfig;

			absl::node_hash_map<std::string, ActorMapping> actorSocketMappingIndex;

			std::hash<std::string> hasher;

#ifdef __linux__

			boost::asio::ip::tcp::acceptor acceptor;

			boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

		};

	}

} // namespace hope
