#pragma once
#include <memory>

#include <async_simple/coro/Lazy.h>

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

	namespace rpc {

		class CoroRpc;

		class CoroRpcHandlerImpl
		{

		public:

			CoroRpcHandlerImpl(hope::signal::WebrtcSignalServer& webrtcSignalServer);

			~CoroRpcHandlerImpl();

			void registerRpcHandler();

		private:

			async_simple::coro::Lazy<RpcForwardResponse> requestForward(RpcForward rpcforward);

		public:

			std::shared_ptr<CoroRpc> coroRpc;

			hope::signal::WebrtcSignalServer& webrtcSignalServer;

		private:

		};

	}

}
