#include "WebRTCSignalData.h"

WebRTCSignalData::WebRTCSignalData(boost::json::object json, std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket, WebRTCSignalServer & webRTCSignalServer)
	:json(json)
	, webrtcSignalSocket(webrtcSignalSocket)
	, webRTCSignalServer(webRTCSignalServer){
	
}