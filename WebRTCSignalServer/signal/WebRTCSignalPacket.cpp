#include "WebRTCSignalPacket.h"

#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"

namespace hope {

	namespace signal {

		WebRTCSignalPacket::WebRTCSignalPacket(std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webRTCSignalManager, int channelIndex)
			: webrtcSignalSocket(webrtcSignalSocket)
			, request(boost::json::make_shared_resource<boost::json::monotonic_resource>())
			, webrtcSignalManager(webRTCSignalManager)
			, channelIndex(channelIndex) {

		}


		WebRTCSignalPacket::WebRTCSignalPacket(WebRTCSignalPacket&& webrtcSignalPacket) noexcept
			: webrtcSignalSocket(std::move(webrtcSignalPacket.webrtcSignalSocket))
			, request(std::move(webrtcSignalPacket.request))
			, requestType(webrtcSignalPacket.requestType)
			, webrtcSignalManager(webrtcSignalPacket.webrtcSignalManager)
			, channelIndex(webrtcSignalPacket.channelIndex) {
		}

		WebRTCSignalPacket& WebRTCSignalPacket::operator=(WebRTCSignalPacket&& webrtcSignalPacket) noexcept {

			this->webrtcSignalSocket = std::move(webrtcSignalPacket.webrtcSignalSocket);

			this->webrtcSignalManager = webrtcSignalPacket.webrtcSignalManager;

			this->request = std::move(webrtcSignalPacket.request);

			this->requestType = webrtcSignalPacket.requestType;

			this->channelIndex = webrtcSignalPacket.channelIndex;

			return *this;

		}

	}

}