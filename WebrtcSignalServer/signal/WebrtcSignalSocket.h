#pragma once

#include <string>
#include <vector>
#include <cstdint>
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

#include "../utils/StringHasher.h"
#include <absl/functional/any_invocable.h>

#include "AsioConcurrentQueue.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

		class WebrtcSignalManager;

		struct FrameRange {

			std::size_t offset = 0;

			std::size_t length = 0;

			bool masked = false;

			std::size_t maskOffset = 0;

			bool final = true;

		};

		struct FrameBatch {

			std::size_t consumed = 0;

			bool closed = false;

		};

		class WebrtcSignalSocket : public std::enable_shared_from_this<WebrtcSignalSocket>
		{
		public:

#ifdef WEBRTC_SIGNAL_SOCKET_DISABLE_SSL

			using WebSocketStream = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

			bool enableSsl = false;
#else

			using WebSocketStream = boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>;

			bool enableSsl = true;

#endif

			WebrtcSignalSocket(boost::asio::io_context& ioContext, WebrtcSignalManager* webrtcSignalManager, int maxTlsHandShakeTime);

			~WebrtcSignalSocket();

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

			StringKeyedNodeMap<int> actorMappingIndex;

		private:

			void closeSocket();

			boost::asio::awaitable<void> reviceCoroutine();

			boost::asio::awaitable<void> writerCoroutine();

			void setTcpKeepAlive(boost::asio::ip::tcp::socket& socket,
				int idle = 0, int intvl = 10, int probes = 10);

#ifdef WEBSOCKET_BIG_BUFFER

			static constexpr std::size_t receiveBufferInitialSize = 65536;

			static constexpr std::size_t receiveBufferMaximumSize = 65536;

			static constexpr std::size_t maximumMessageSize = 65536;

#else

			static constexpr std::size_t receiveBufferInitialSize = 8192;

			static constexpr std::size_t receiveBufferMaximumSize = 16384;

			static constexpr std::size_t maximumMessageSize = 16384;

#endif

			static constexpr std::size_t maximumFramesPerWrite = 32;

			static constexpr std::size_t maximumFrameHeaderSize = 10;

			static FrameBatch takeFrames(const char* data, std::size_t size, std::vector<FrameRange>& frames);

			static std::size_t encodeFrameHeader(char* out, std::size_t length, bool binary);

			static void unmaskPayload(char* payload, std::size_t length, const char* maskKey);

		private:

			WebrtcSignalManager * webrtcSignalManager;

			boost::asio::io_context& ioContext;

			WebSocketStream webSocket;

			boost::asio::ip::tcp::resolver resolver;

			AsioConcurrentQueue<std::string> asioConcurrentQueue;

			std::atomic<bool> asyncEvents{ false };

			std::string sessionId;

			std::string accountId;

			std::atomic<bool> isHandleDisConnect{ false };

			std::chrono::milliseconds handshakeTimeout{ 10000 };

			std::vector<char> receiveBuffer;

			std::vector<FrameRange> receiveFrameRanges;

			std::size_t receiveHeldBytes{ 0 };

			std::vector<char> frameHeaderScratch;

		private:

			absl::AnyInvocable<void(std::string, std::string)> onDisConnectHandle;

		};
	}
}