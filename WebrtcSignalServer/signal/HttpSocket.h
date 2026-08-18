#pragma once

#include <memory>

#include <boost/asio.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>

namespace hope {

	namespace signal {

		class WebrtcSignalManager;

		class HttpSocket : public std::enable_shared_from_this<HttpSocket>
		{
		public:

			HttpSocket(boost::asio::io_context& ioContext, WebrtcSignalManager* webrtcSignalManager, int maxTlsHttpHandShakeTime, int maxHttpKeepAliveTime);

			~HttpSocket();

			boost::asio::ip::tcp::socket& getSocket();

			boost::asio::awaitable<void> asyncEvent();

			boost::asio::awaitable<bool> asyncHandShake();

			boost::asio::awaitable<bool> asyncWrite(boost::beast::http::response<boost::beast::http::string_body> httpResponse);

			boost::asio::awaitable<boost::beast::http::request<boost::beast::http::string_body>> asyncRead();

			void asyncReadKeepAlive(boost::beast::http::request<boost::beast::http::string_body>& httpRequest);

			void closeSocket();

			boost::asio::io_context& getIoContext();

			WebrtcSignalManager* getWebrtcSignalManager();

			bool getKeepAlive();

		private:

		public:

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			bool enableSsl = false;

#else

			bool enableSsl = true;

#endif

		private:

			boost::asio::io_context& ioContext;

			WebrtcSignalManager * webrtcSignalManager;

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			boost::beast::tcp_stream tcpStream;

#else

			boost::beast::ssl_stream<boost::asio::ip::tcp::socket> sslStream;

#endif

			std::chrono::steady_clock::time_point lastKeepAliveTime;

			std::atomic<bool> isClosed{ false };

			std::atomic<bool> keepAliveRunning{ false };

			bool isKeepAlive;

			std::chrono::seconds timeoutSec;

			std::chrono::milliseconds handshakeTimeout{ 10000 };

			int maxHttpKeepAliveTimeSec;

			boost::asio::steady_timer keepTimer;

		};

	}

}
