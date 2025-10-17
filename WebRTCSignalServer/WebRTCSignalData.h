#pragma once
#include <memory>
#include <boost/json.hpp>
#include "WebRTCSignalSocket.h"
#include "WebRTCSignalServer.h"

class WebRTCSignalData {

public:

	WebRTCSignalData(boost::json::object json, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalServer& webRTCSignalServer);

	std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket;

	boost::json::object json;

	WebRTCSignalServer& webRTCSignalServer;

};

