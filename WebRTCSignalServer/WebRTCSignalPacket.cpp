#include "WebRTCSignalPacket.h"

#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"

namespace hope {

	namespace core {

		WebRTCSignalPacket::WebRTCSignalPacket(std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webRTCSignalManager, int channelIndex)
			: webrtcSignalSocket(webrtcSignalSocket)
			, request(boost::json::make_shared_resource<boost::json::monotonic_resource>())
			, webrtcSignalManager(webRTCSignalManager)
			, channelIndex(channelIndex) {

		}

	}

}