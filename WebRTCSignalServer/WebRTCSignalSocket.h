#pragma once
#include <string>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp> 
#include <boost/beast.hpp>

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
#include <absl/functional/any_invocable.h>

#include "AsioConcurrentQueue.h"

namespace hope {

	namespace core {

		class WebRTCSignalServer;

		class WebRTCSignalManager;

		class WebRTCSignalSocket : public std::enable_shared_from_this<WebRTCSignalSocket>
		{
		public:

#ifdef WEBRTC_SIGNAL_SOCKET_DISABLE_SSL

			using WebSocketStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

			bool enableSsl = false;
#else

			using WebSocketStream = boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

			bool enableSsl = true;

#endif

			static void initSslContext();

			static boost::asio::ssl::context& getSslContext();

			WebRTCSignalSocket(boost::asio::io_context& ioContext, WebRTCSignalManager* webrtcSignalManager);

			~WebRTCSignalSocket();

			boost::asio::ip::tcp::socket& getSocket();

			WebSocketStream& getWebSocket();

			boost::asio::awaitable<bool> handShake();

			boost::asio::io_context& getIoCompletionPorts();

			void asyncEvent();

			void closeEvent();

			void asyncWrite(std::string packet);

			void setAccountId(const std::string& accountId);

			std::string getAccountId();

			std::string getSessionId();

			std::string getRemoteAddress();

		public:

			void setOnDisConnectHandle(absl::AnyInvocable<void(std::string, std::string)>&& handle);

		public:

			absl::flat_hash_map<std::string, int> actorMappingIndex;

		private:

			void closeSocket();

			boost::asio::awaitable<void> reviceCoroutine();

			boost::asio::awaitable<void> writerCoroutine();

			void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket,
				int idle = 0, int intvl = 3, int probes = 3);

		private:

			WebRTCSignalManager* webrtcSignalManager;

			boost::asio::io_context& ioContext;

			WebSocketStream webSocket;

			boost::asio::ip::tcp::resolver resolver;

			AsioConcurrentQueue<std::string> asioConcurrentQueue;

			std::atomic<bool> asyncEvents{ false };

			std::string sessionId;

			std::string accountId;

			std::atomic<bool> isHandleDisConnect{ false };

			static boost::asio::ssl::context sslContext;

		private:

			absl::AnyInvocable<void(std::string, std::string)> onDisConnectHandle;

		};
	}
}