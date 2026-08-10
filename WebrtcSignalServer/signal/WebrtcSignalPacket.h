#pragma once
#include <memory>

#include <boost/json.hpp>

namespace hope {

	namespace signal {

		class WebrtcSignalSocket;

		class WebrtcSignalManager;

		class WebrtcSignalPacket {

		public:

			WebrtcSignalPacket(std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket, WebrtcSignalManager* webrtcSignalManager, int channelIndex);

			WebrtcSignalPacket(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket& operator=(const WebrtcSignalPacket& webrtcSignalPacket) = delete;

			WebrtcSignalPacket(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			WebrtcSignalPacket& operator=(WebrtcSignalPacket&& webrtcSignalPacket) noexcept;

			std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket;

			boost::json::object request;

			int requestType = 0;

			WebrtcSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}

