#include "WebrtcSignalPacket.h"

#include "WebrtcSignalSocket.h"
#include "WebrtcSignalManager.h"

namespace hope {

	namespace signal {

		WebrtcSignalPacket::WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager* webrtcSignalManager, int channelIndex)
			: webrtcSignalSocket(webrtcSignalSocket)
			, request(boost::json::make_shared_resource<boost::json::monotonic_resource>())
			, webrtcSignalManager(webrtcSignalManager)
			, channelIndex(channelIndex) {

		}


		WebrtcSignalPacket::WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept
			: webrtcSignalSocket(std::move(webrtcSignalPacket.webrtcSignalSocket))
			, request(std::move(webrtcSignalPacket.request))
			, requestType(webrtcSignalPacket.requestType)
			, webrtcSignalManager(webrtcSignalPacket.webrtcSignalManager)
			, channelIndex(webrtcSignalPacket.channelIndex) {
		}

		WebrtcSignalPacket& WebrtcSignalPacket::operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept {

			this->webrtcSignalSocket = std::move(webrtcSignalPacket.webrtcSignalSocket);

			this->webrtcSignalManager = webrtcSignalPacket.webrtcSignalManager;

			this->request = std::move(webrtcSignalPacket.request);

			this->requestType = webrtcSignalPacket.requestType;

			this->channelIndex = webrtcSignalPacket.channelIndex;

			return *this;

		}

	}

}