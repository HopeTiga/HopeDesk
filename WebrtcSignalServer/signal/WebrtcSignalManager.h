#pragma once
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include <absl/container/node_hash_map.h>

#include "WebrtcLogicSystem.h"
#include "WebrtcSignalSocket.h"
#include "HttpSocket.h"

#include "AwaitableTask.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

		struct WebrtcSignalChannelConfig {

			size_t hashSize = 1;          // actor 路由分桶数(= 通道数 threadSize)

			int threshold = 256;          // 全局任务队列高水位

			int exitThreshold = 128;      // 本地队列高水位,触发切本地处理

			int asyncThreshold = 32;      // 异步派发阈值

			int socketWaitTime = 10000; // WebSocket 握手超时(ms)

		};

		class WebrtcSignalManager : public std::enable_shared_from_this<WebrtcSignalManager>
		{
			friend class WebrtcLogicSystem;
		public:

			WebrtcSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebrtcSignalServer* webrtcSignalServer, TaskChannel& taskQueues, WebrtcSignalChannelConfig channelConfig);

			~WebrtcSignalManager();

			std::shared_ptr<hope::signal::WebrtcLogicSystem> getLogicSystem();

			void removeConnection(std::string accountId, std::string sessionId);

			int getChannelIndex();

			boost::asio::io_context& getIoCompletionPorts();

			std::shared_ptr<hope::signal::WebrtcSignalSocket> generateWebRTCSignalSocket();

			std::shared_ptr<HttpSocket> generateHttpSocket();

			void registerSocket(const std::string& accountId, std::shared_ptr<WebrtcSignalSocket> socket);

			WebrtcSignalServer* getWebrtcSignalServer();

#ifdef __linux__

			void asyncAccept(std::atomic<bool>& runAccepct, boost::asio::ip::tcp::endpoint endpoint, boost::asio::ip::tcp::endpoint httpEndpoint, int enableHttp = 0);

#endif

		public:

			struct ActorMapping
			{

				std::string sessionId;

				int channelIndex;

			};

			absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>> webrtcSocketMap;

			size_t hashSize;

			absl::node_hash_map<std::string, ActorMapping> actorSocketMappingIndex;

			std::hash<std::string> hasher;

		private:

			boost::asio::io_context& ioContext;

			WebrtcSignalServer* webrtcSignalServer;

			size_t channelIndex;

			std::shared_ptr<WebrtcLogicSystem> webrtcLogicSystem;

			WebrtcSignalChannelConfig channelConfig;


#ifdef __linux__

			boost::asio::ip::tcp::acceptor acceptor;

			boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

		};

	}

} // namespace hope
