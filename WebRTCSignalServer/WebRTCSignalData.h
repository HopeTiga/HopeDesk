#pragma once
#include <memory>
#include <boost/json.hpp>

namespace Hope {

	class WebRTCSignalSocket;
	class WebRTCSignalManager;

	class WebRTCSignalData {

	public:

		WebRTCSignalData(boost::json::object json, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalManager* webRTCSignalManager);

		std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

		boost::json::object json;

		WebRTCSignalManager* webrtcSignalManager;

	};
}

