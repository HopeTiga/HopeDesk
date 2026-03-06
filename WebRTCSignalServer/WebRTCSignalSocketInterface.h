#pragma once
#include <string>
#include <functional>

namespace hope {

	namespace core {

		enum class SocketType
		{
			WebSocket = 0
		};
	
		enum class LogicSocketType
		{
			WebRTCUserSocket = 0,
			CloudGameServerSocket = 1,
			CloudGameProcessSocket = 2,
			CloudDeviceSocket = 3,
			CloudDeviceProcessSocket = 4
		};


		class WebRTCSignalSocketInterface {

		public:

			virtual ~WebRTCSignalSocketInterface() = default;

			virtual void writeAsync(unsigned char* data, size_t size) = 0;

			virtual void writeAsync(std::string str) = 0;

			virtual void writeAsync(std::string str,std::function<void()> func) = 0;

			virtual void setLogicSocketType(LogicSocketType type) = 0;

			virtual LogicSocketType getLogicSocketType() = 0;

			virtual std::string getSessionId() = 0;

			virtual std::string getRemoteAddress() = 0;

			virtual SocketType getSocketType() = 0;

			virtual void closeSocket() = 0;

		};

	}

}