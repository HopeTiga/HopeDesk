#pragma once
#include <memory>

#include <async_simple/coro/Lazy.h>

#include "Rpc.h"
#include "CoroRpcHandleInterface.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

	}

	namespace rpc {

		class CoroRpcHandleImpl : public CoroRpcHandleInterface
		{

		public:

			CoroRpcHandleImpl(hope::signal::WebrtcSignalServer& webrtcSignalServer);

			~CoroRpcHandleImpl();

			void registerRpcHandle();

		public:

			async_simple::coro::Lazy<RpcForwardResponse> requestForward(RpcForward rpcforward);

		private:

		};

	}

}

