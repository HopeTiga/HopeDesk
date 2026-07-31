#pragma once
#include <string>

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