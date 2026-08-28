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
			, packet(std::move(webrtcSignalPacket.packet))
			, webrtcEnvelope(webrtcSignalPacket.webrtcEnvelope)
			, webrtcSignalManager(webrtcSignalPacket.webrtcSignalManager)
			, channelIndex(webrtcSignalPacket.channelIndex) {
		}

		WebrtcSignalPacket& WebrtcSignalPacket::operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept {

			this->webrtcSignalSocket = std::move(webrtcSignalPacket.webrtcSignalSocket);

			this->webrtcSignalManager = webrtcSignalPacket.webrtcSignalManager;

			this->packet = std::move(webrtcSignalPacket.packet);

			this->webrtcEnvelope = webrtcSignalPacket.webrtcEnvelope;

			this->channelIndex = webrtcSignalPacket.channelIndex;

			return *this;

		}

	}

}