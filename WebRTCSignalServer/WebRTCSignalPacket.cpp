#include "WebRTCSignalPacket.h"

#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"

namespace hope {

	namespace core {

		WebRTCSignalPacket::WebRTCSignalPacket(WebRTCSignalRequest webrtcSignalRequest, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webrtcSignalManager, int channelIndex)
			: webrtcSignalRequest(webrtcSignalRequest)
			, webrtcSignalSocket(webrtcSignalSocket)
			, webrtcSignalManager(webrtcSignalManager)
			, channelIndex(channelIndex) {

		}

	}

}