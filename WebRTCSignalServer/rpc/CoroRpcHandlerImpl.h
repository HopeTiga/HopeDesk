#pragma once
#include <memory>

#include <async_simple/coro/Lazy.h>

namespace hope {

	namespace signal {

		class WebRTCSignalServer;

	}

	namespace rpc {

		class CoroRpc;

		class CoroRpcHandlerImpl
		{

		public:

			CoroRpcHandlerImpl(hope::signal::WebRTCSignalServer& webrtcSignalServer);

			~CoroRpcHandlerImpl();

			void registerRpcHandler();

		private:

			async_simple::coro::Lazy<int> rpcEcho(int value);

		public:

			std::shared_ptr<CoroRpc> coroRpc;

			hope::signal::WebRTCSignalServer& webrtcSignalServer;

		private:

		};

	}

}
