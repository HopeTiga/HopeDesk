#include "CoroRpcHandlerImpl.h"
#include "CoroRpc.h"

#include "../signal/WebRTCSignalServer.h"

#include "../utils/Utils.h"

namespace hope {

	namespace rpc {

		CoroRpcHandlerImpl::CoroRpcHandlerImpl(hope::signal::WebRTCSignalServer& webrtcSignalServer)
			: webrtcSignalServer(webrtcSignalServer)
			, coroRpc(nullptr){

		}

		CoroRpcHandlerImpl::~CoroRpcHandlerImpl() {

	

		}

		void CoroRpcHandlerImpl::registerRpcHandler() {

			if (!coroRpc) return;

			coroRpc->registerHandler<&CoroRpcHandlerImpl::rpcEcho>(this);
			
		}

		async_simple::coro::Lazy<int> CoroRpcHandlerImpl::rpcEcho(int value) {

			LOG_INFO("rpcEcho value: %d", value);

			LOG_INFO("response value: %d", webrtcSignalServer.getChannelNumbers());

			co_return static_cast<int>(webrtcSignalServer.getChannelNumbers());

		}

	}

}