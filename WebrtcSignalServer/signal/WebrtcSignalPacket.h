#pragma once
#include <memory>
#include <string>

namespace hope {

	namespace signal {

		class WebrtcSignalSocket;

		class WebrtcSignalManager;

		struct WebrtcRequest {

			int requestType = 0;

			std::string accountId;

			std::string targetId;

			std::string payload;

		};

		struct WebrtcResponse {

			int requestType = 0;

			int state = 0;

			std::string message;

			std::string accountId;

			std::string targetId;

			std::string payload;

		};

		class WebrtcSignalPacket {

		public:

			WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager* webrtcSignalManager, int channelIndex);

			WebrtcSignalPacket(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket& operator=(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			WebrtcSignalPacket& operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket;

			WebrtcRequest webrtcRequest;

			int requestType = 0;

			WebrtcSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}
