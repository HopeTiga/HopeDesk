#pragma once
#include <memory>

#include <boost/json.hpp>

namespace hope {

	namespace signal {

		class WebRTCSignalSocket;

		class WebRTCSignalManager;

		class WebRTCSignalPacket {

		public:

			WebRTCSignalPacket(std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webrtcSignalManager, int channelIndex);

			WebRTCSignalPacket(const WebRTCSignalPacket& webrtcSignalPacket) = delete;

			WebRTCSignalPacket& operator=(const WebRTCSignalPacket& webrtcSignalPacket) = delete;

			WebRTCSignalPacket(WebRTCSignalPacket&& webrtcSignalPacket) noexcept;

			WebRTCSignalPacket& operator=(WebRTCSignalPacket&& webrtcSignalPacket) noexcept;

			std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

			boost::json::object request;

			int requestType = 0;

			WebRTCSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}

