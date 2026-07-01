#pragma once
#include <memory>

#include <boost/json.hpp>

namespace hope {

	namespace core {

		class WebRTCSignalSocket;

		class WebRTCSignalManager;

		class WebRTCSignalPacket{

		public:

			WebRTCSignalPacket(std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webrtcSignalManager, int channelIndex);

			WebRTCSignalPacket(const WebRTCSignalPacket& webrtcSignalPacket) = delete;

			WebRTCSignalPacket& operator=(const WebRTCSignalPacket& webrtcSignalPacket) = delete;

			WebRTCSignalPacket(WebRTCSignalPacket&& webrtcSignalPacket) noexcept ;

			WebRTCSignalPacket& operator=(WebRTCSignalPacket&& webrtcSignalPacket) noexcept ;

			std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

			// Parsed request body. Constructed with an owning, refcounted
			// storage_ptr (make_shared_resource<monotonic_resource>), so the
			// arena backing it travels with the object: moving `request` into
			// another coroutine keeps the arena alive (refcounted) until that
			// holder is destroyed. No external lifetime management needed.
			boost::json::object request;

			WebRTCSignalManager* webrtcSignalManager;

			int channelIndex;

		};
	}

}

