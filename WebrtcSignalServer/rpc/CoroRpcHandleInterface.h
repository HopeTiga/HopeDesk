#pragma once

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

	}

	namespace rpc {

		class CoroRpcHandleInterface
		{

		public:

			CoroRpcHandleInterface(hope::signal::WebrtcSignalServer& webrtcSignalServer)
				: webrtcSignalServer(webrtcSignalServer) {

			}

			virtual ~CoroRpcHandleInterface() = default;

			CoroRpcHandleInterface(const CoroRpcHandleInterface&) = delete;

			CoroRpcHandleInterface& operator=(const CoroRpcHandleInterface&) = delete;

			virtual void registerRpcHandle() = 0;

		public:

			hope::signal::WebrtcSignalServer& webrtcSignalServer;

		};

	}

}
