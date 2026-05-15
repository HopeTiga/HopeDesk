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

	namespace core {

		class WebRTCSignalServer;

		class WebRTCSignalManager : public std::enable_shared_from_this<WebRTCSignalManager>
		{
			friend class hope::core::WebRTCLogicSystem;
		public:

			WebRTCSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebRTCSignalServer* webrtcSignalServer, TaskChannel& taskQueues);

			~WebRTCSignalManager();

			std::shared_ptr<hope::core::WebRTCLogicSystem> getLogicSystem();

			void removeConnection(std::string accountId, std::string sessionId);

			int getChannelIndex();

			boost::asio::io_context& getIoCompletionPorts();

			std::shared_ptr<hope::core::WebRTCSignalSocket> generateWebRTCSignalSocket();

			std::shared_ptr<HttpSocket> generateHttpSocket(bool enableSsl = false);

#ifdef __linux__

			void asyncAccept(boost::asio::ip::tcp::endpoint endpoint, std::atomic<bool>& runAccepct, boost::asio::ip::tcp::endpoint httpEndpoint, int enableHttp = 0);

#endif

		private:

			boost::asio::io_context& ioContext;

			WebRTCSignalServer* webrtcSignalServer;

			size_t channelIndex;

			std::shared_ptr<hope::core::WebRTCLogicSystem> logicSystem;

			absl::node_hash_map<std::string, std::shared_ptr<WebRTCSignalSocket>> webrtcSocketMap;

			size_t hashSize = std::thread::hardware_concurrency();

			absl::node_hash_map<std::string, int> actorSocketMappingIndex;

			std::hash<std::string> hasher;

#ifdef __linux__

			boost::asio::ip::tcp::acceptor acceptor;

			boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

		};

	}

} // namespace hope
