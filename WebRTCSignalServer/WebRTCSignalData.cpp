#include "WebRTCSignalData.h"

#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"

namespace Hope {
	WebRTCSignalData::WebRTCSignalData(boost::json::object json, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webrtcSignalManager)
		:json(json)
		, webrtcSignalSocket(webrtcSignalSocket)
		, webrtcSignalManager(webrtcSignalManager) {

	}
}