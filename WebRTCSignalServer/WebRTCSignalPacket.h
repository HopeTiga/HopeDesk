#pragma once
#include <memory>

#include <boost/json.hpp>

namespace hope {

	namespace core {

		class WebRTCSignalSocket;

		class WebRTCSignalManager;

		class WebRTCSignalPacket : public std::enable_shared_from_this<WebRTCSignalPacket> {

		public:

			WebRTCSignalPacket(boost::json::object && request, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webRTCSignalManager, int channelIndex);

			std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

			boost::json::object request;

			WebRTCSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}

