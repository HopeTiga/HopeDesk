#pragma once
#include <string>
#include <functional>

namespace hope {

	namespace core {

		enum class SocketType
		{
			WebSocket = 0
		};

		class WebRTCSignalSocketInterface {

		public:

			virtual ~WebRTCSignalSocketInterface() = default;

			virtual void asyncWrite(unsigned char* data, size_t size) = 0;

			virtual void asyncWrite(std::string str) = 0;

			virtual std::string getSessionId() = 0;

			virtual std::string getRemoteAddress() = 0;

			virtual SocketType getSocketType() = 0;

			virtual void closeSocket() = 0;

		};

	}

}