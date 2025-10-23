#pragma once
#include "WebRTCSignalServer.h"
#include "WebRTCSignalSocket.h"
#include <vector>

#include <boost/asio.hpp>

#include <tbb/concurrent_unordered_map.h>




namespace Hope {

	class WebRTCSignalServer;

	class WebRTCSignalManager : public std::enable_shared_from_this<WebRTCSignalManager>
	{
	public:

		WebRTCSignalManager(boost::asio::io_context & ioContext,int channelIndex,WebRTCSignalServer * webrtcSignalServer);

		~WebRTCSignalManager();

		std::shared_ptr<WebRTCSignalSocket> generateWebRTCSignalSocket();

		boost::asio::io_context& getIoComplatePorts();

		void handleMessage(boost::json::object message, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket);

		void removeConnection(const std::string& accountID);

		tbb::concurrent_unordered_map<std::string,int>& getActorSocketMappingIndex();

	private:

		boost::asio::io_context & ioContext;

		int channelIndex;

		std::unordered_map<std::string, std::shared_ptr<WebRTCSignalSocket>> webrtcSignalSocketMap;

		WebRTCSignalServer* webrtcSignalServer;

		size_t hashSize = std::thread::hardware_concurrency() * 2;

		tbb::concurrent_unordered_map<std::string, int> actorSocketMappingIndex;

		std::hash<std::string> hasher;
	};
}

