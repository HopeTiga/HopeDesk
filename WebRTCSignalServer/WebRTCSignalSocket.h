#pragma once
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp> 
#include <boost/beast.hpp>
#include <boost/json.hpp>
#ifdef _WIN32
#include <winsock2.h>      // Windows Socket API
#include <ws2tcpip.h>      // Windows Socket 扩展
#include <mstcpip.h>       // SIO_KEEPALIVE_VALS 和 tcp_keepalive 结构体
#pragma comment(lib, "ws2_32.lib")
#elif defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <absl/container/flat_hash_map.h>

#include "WebRTCSignalSocketInterface.h"
#include "AsioConcurrentQueue.h"

namespace hope {

	namespace core {

		class WebRTCSignalServer;

		class WebRTCSignalManager;

		class WebRTCSignalSocket : public std::enable_shared_from_this<WebRTCSignalSocket>
		{
		public:

			static void initSslContext(); // 初始化一次

			static boost::asio::ssl::context& getSslContext();

			WebRTCSignalSocket(boost::asio::io_context& ioContext, WebRTCSignalManager* webrtcSignalManager);

			~WebRTCSignalSocket();

			boost::asio::ip::tcp::socket& getSocket();

			boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>& getWebSocket();

			boost::asio::awaitable<void> handShake();

			boost::asio::io_context& getIoCompletionPorts();

			void runEventLoop();

			void clear();

			virtual void asyncWrite(unsigned char* data, size_t size);

			void asyncWrite(std::string str);

			void setAccountId(const std::string& accountId);

			std::string getAccountId();

			void setRegistered(bool isRegistered);

			void destroy();

			std::string getSessionId();

			std::string getRemoteAddress();

			void closeSocket();

		public:

			void setOnDisConnectHandle(std::function<void(std::string, std::string)> handle);

		public:

			absl::flat_hash_map<std::string, int> actorMappingIndex;

		private:

			boost::asio::awaitable<void> registrationTimeout();

			boost::asio::awaitable<void> reviceCoroutine();

			boost::asio::awaitable<void> writerCoroutine();

			void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket,
				int idle = 0, int intvl = 3, int probes = 3);
		private:

			WebRTCSignalManager* webrtcSignalManager;

			boost::asio::io_context& ioContext;

			boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> webSocket;

			boost::asio::ip::tcp::resolver resolver;

			AsioConcurrentQueue<std::string> asioConcurrentQueue;

			std::atomic<bool> webSocketRuns{ false };

			std::string accountId;

			boost::asio::steady_timer registrationTimer; // 计时器成员

			std::atomic<bool> isRegistered{ false }; // 新增：注册状态标志

			std::atomic<bool> isStop{ false };

			std::atomic<bool> isHandleDisConnect{ false };

			std::atomic<bool> isDeleted{ false };

			std::atomic<bool> writerCoroutineRuns{ false };

			std::string sessionId;

			static boost::asio::ssl::context sslContext;

		private:

			std::function<void(std::string, std::string)> onDisConnectHandle;

		};
	}
}