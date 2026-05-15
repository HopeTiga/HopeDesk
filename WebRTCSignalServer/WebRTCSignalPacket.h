#pragma once
#include <memory>

#include "WebRTCSignalRequest.h"

namespace hope {

	namespace core {

		class WebRTCSignalSocket;

		class WebRTCSignalManager;

		class WebRTCSignalPacket : public std::enable_shared_from_this<WebRTCSignalPacket> {

		public:

			WebRTCSignalPacket(WebRTCSignalRequest webrtcSignalRequest, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webRTCSignalManager, int channelIndex);

			std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

			WebRTCSignalRequest webrtcSignalRequest;

			WebRTCSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}

