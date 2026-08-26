#include "WebrtcSignalPacket.h"

#include "WebrtcSignalSocket.h"
#include "WebrtcSignalManager.h"

#include "../utils/Utils.h"

namespace hope {

	namespace signal {

		WebrtcSignalPacket::WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager* webrtcSignalManager, int channelIndex)
			: webrtcSignalSocket(webrtcSignalSocket)
			, webrtcSignalManager(webrtcSignalManager)
			, channelIndex(channelIndex) {

		}


		WebrtcSignalPacket::WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept
			: webrtcSignalSocket(std::move(webrtcSignalPacket.webrtcSignalSocket))
			, webrtcRequest(std::move(webrtcSignalPacket.webrtcRequest))
			, requestType(webrtcSignalPacket.requestType)
			, webrtcSignalManager(webrtcSignalPacket.webrtcSignalManager)
			, channelIndex(webrtcSignalPacket.channelIndex) {
		}

		WebrtcSignalPacket& WebrtcSignalPacket::operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept {

			this->webrtcSignalSocket = std::move(webrtcSignalPacket.webrtcSignalSocket);

			this->webrtcSignalManager = webrtcSignalPacket.webrtcSignalManager;

			this->webrtcRequest = std::move(webrtcSignalPacket.webrtcRequest);

			this->requestType = webrtcSignalPacket.requestType;

			this->channelIndex = webrtcSignalPacket.channelIndex;

			return *this;

		}

	}

}