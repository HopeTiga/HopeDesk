#pragma once
#include <memory>

#include <async_simple/coro/Lazy.h>

#include "Rpc.h"

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

		public:

			async_simple::coro::Lazy<RpcForwardResponse> requestForward(RpcForward rpcforward);

		public:

			CoroRpc * coroRpc;

			hope::signal::WebrtcSignalServer& webrtcSignalServer;

		private:

		};

	}

}
