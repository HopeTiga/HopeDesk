#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace hope {

	namespace signal {

		class WebrtcSignalSocket;

		class WebrtcSignalManager;

		struct WebrtcEnvelopeView {

			int requestType = 0;

			int state = 0;

			std::string_view message;

			std::string_view accountId;

			std::string_view targetId;

		};

		class WebrtcSignalPacket {

		public:

			WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager* webrtcSignalManager, int channelIndex);

			WebrtcSignalPacket(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket& operator=(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			WebrtcSignalPacket& operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket;

			std::string packet;

			WebrtcEnvelopeView webrtcEnvelope;

			WebrtcSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}
