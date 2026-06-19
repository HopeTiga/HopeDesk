#include "WebRTCSignalPacket.h"

#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"

namespace hope {

	namespace core {

		WebRTCSignalPacket::WebRTCSignalPacket(boost::json::object && request, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webrtcSignalManager, int channelIndex)
			: request(request)
			, webrtcSignalSocket(webrtcSignalSocket)
			, webrtcSignalManager(webrtcSignalManager)
			, channelIndex(channelIndex) {

		}

	}

}