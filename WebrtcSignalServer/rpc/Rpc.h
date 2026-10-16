#pragma once
#include <string>
#include <memory>

struct RpcForward
{
	int forwardChannel;

	std::string forwardPacket;

};

struct RpcForwardResponse
{
	int state;

	std::string message;

};

namespace hope {
	namespace signal {
		class WebrtcSignalServer;
	}
}

void initCoroRpcHandleInterface(hope::signal::WebrtcSignalServer * webrtcSignalServer);