
#include "HttpSocket.h"

#include <chrono>

#include "../ssl/Ssl.h"

#include "WebrtcSignalManager.h"
#include "WebrtcSignalSocket.h"

#include "../utils/Utils.h"
#include "../utils/CompletionHandle.h"

namespace hope {

	namespace signal {

		HttpSocket::HttpSocket(boost::asio::io_context& ioContext, WebrtcSignalManager* webrtcSignalManager, int maxTlsHttpHandShakeTime, int maxHttpKeepAliveTime)
			: ioContext(ioContext)
			, webrtcSignalManager(webrtcSignalManager)
#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			, tcpStream(ioContext)

#else

			, sslStream(ioContext, getSslContext())

#endif
			, timeoutSec(std::chrono::seconds(maxTlsHttpHandShakeTime / 1000))
			, handshakeTimeout(maxTlsHttpHandShakeTime)
			, maxHttpKeepAliveTimeSec(maxHttpKeepAliveTime)
			, keepTimer(ioContext)
		{

			LOG_INFO("HttpSocket");

		}

		HttpSocket::~HttpSocket()
		{
			closeSocket();
			LOG_INFO("~HttpSocket");
		}

		boost::asio::ip::tcp::socket& HttpSocket::getSocket() {

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			return tcpStream.socket();

#else

			return sslStream.next_layer();

#endif

		}

		boost::asio::awaitable<void> HttpSocket::asyncEvent()
		{
			if (!co_await asyncHandShake()) {

				LOG_ERROR("AsyncHandshake Failed, Close Socket.");

#if !defined(WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL)

				if (enableSsl) {

					boost::system::error_code ec;

					co_await sslStream.async_shutdown(boost::asio::redirect_error(boost::asio::deferred, ec));

					if (ec) LOG_ERROR("SSL AsyncShutdown Failed: {}", ec.message().c_str());

				}

#endif

				closeSocket();

				co_return;
			}

			try {

				HttpReadResult readResult = co_await asyncRead();

				if (!readResult.succeeded) {

					closeSocket();

					co_return;
				}

				webrtcSignalManager->getLogicSystem()->postHttpTask(shared_from_this(), readResult.httpRequest);

				asyncReadKeepAlive(readResult.httpRequest);

			}
			catch (std::exception& e) {

				LOG_ERROR("AsyncRead or PostHttpTask or AsyncReadKeepAlive Exception: {}", e.what());

				closeSocket();

			};

			co_return;
		}


		boost::asio::awaitable<bool> HttpSocket::asyncHandShake()
		{
			if (!enableSsl) co_return true;

#if !defined(WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL)

			std::chrono::milliseconds timeout = handshakeTimeout;

			try {
				boost::system::error_code ec;

				boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
				timer.expires_after(timeout);

				bool isTimeout = false;
				timer.async_wait([&](const boost::system::error_code& error)mutable {
					if (!error) {
						isTimeout = true;
						sslStream.lowest_layer().cancel();
					}
					});

				std::tuple<boost::system::error_code> op = co_await sslStream.async_handshake(
					boost::asio::ssl::stream_base::server,
					boost::asio::as_tuple(boost::asio::deferred)
				);

				timer.cancel();

				auto [handshake_ec] = std::move(op);

				if (isTimeout) {
					LOG_ERROR("AsyncHandShake Timeout after {}ms", timeout.count());
					co_return false;
				}

				if (handshake_ec) {
					LOG_ERROR("AsyncHandShake Error: {}", handshake_ec.message().c_str());
					co_return false;
				}

			}
			catch (const std::exception& e) {
				LOG_ERROR("AsyncHandShake Exception: {}", e.what());
				co_return false;
			}

#endif

			co_return true;
		}

		boost::asio::awaitable<HttpReadResult> HttpSocket::asyncRead()
		{
			boost::beast::flat_buffer buffer;
			HttpReadResult readResult;
			boost::system::error_code ec;

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			// Read idle timeout: prevents a client that finished TLS but sends nothing from holding the coroutine and socket forever
			boost::asio::steady_timer readTimer(co_await boost::asio::this_coro::executor);
			readTimer.expires_after(timeoutSec);
			bool isTimeout = false;
			readTimer.async_wait([&](const boost::system::error_code& tec) {
				if (!tec) {
					isTimeout = true;
					tcpStream.socket().cancel();
				}
			});

			co_await boost::beast::http::async_read(tcpStream, buffer, readResult.httpRequest, boost::asio::redirect_error(boost::asio::deferred, ec));

			readTimer.cancel();

			if (isTimeout || ec) {
				LOG_ERROR("AsyncRead Failed: {}", ec.message().c_str());
				co_return readResult;
			}

#else

			// Read idle timeout: prevents a client that finished TLS but sends nothing from holding the coroutine and socket forever
			boost::asio::steady_timer readTimer(co_await boost::asio::this_coro::executor);
			readTimer.expires_after(timeoutSec);
			bool isTimeout = false;
			readTimer.async_wait([&](const boost::system::error_code& tec) {
				if (!tec) {
					isTimeout = true;
					sslStream.lowest_layer().cancel();
				}
			});

			co_await boost::beast::http::async_read(sslStream, buffer, readResult.httpRequest, boost::asio::redirect_error(boost::asio::deferred, ec));

			readTimer.cancel();

			if (isTimeout || ec) {
				LOG_ERROR("SSL AsyncRead Failed: {}", ec.message().c_str());
				co_return readResult;
			}

#endif

			readResult.succeeded = true;

			co_return readResult;

		}

		void HttpSocket::asyncReadKeepAlive(boost::beast::http::request<boost::beast::http::string_body>& httpRequest)
		{

			isKeepAlive = httpRequest.keep_alive();

			if (!isKeepAlive) {

				keepTimer.cancel();

				return;
			}

			boost::beast::http::request<boost::beast::http::string_body>::iterator it = httpRequest.find("Keep-Alive");
			if (it != httpRequest.end()) {
				std::string value = it->value().data();
				size_t pos = value.find("timeout=");
				if (pos != std::string::npos) {
					pos += 8; // strlen("timeout=")
					size_t end = value.find(',', pos);
					try {
						int sec = std::stoi(value.substr(pos, end - pos));
						if (sec > 0) {
							timeoutSec = std::chrono::seconds((maxHttpKeepAliveTimeSec > 0 && sec > maxHttpKeepAliveTimeSec) ? maxHttpKeepAliveTimeSec : sec);
						}
					}
					catch (...) {
						LOG_WARN("Failed To Parse Keep-Alive Timeout Value: {}", value.c_str());
					}
				}
			}

			std::chrono::steady_clock::time_point newExpireTime = std::chrono::steady_clock::now() + timeoutSec;

			lastKeepAliveTime = newExpireTime;

			if (!keepAliveRunning.exchange(true)) {

				boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {

					std::chrono::steady_clock::time_point lastTime = self->lastKeepAliveTime;

					while (self->isKeepAlive) {
						self->keepTimer.expires_at(lastTime); 
						boost::system::error_code ec;
						co_await self->keepTimer.async_wait(boost::asio::redirect_error(boost::asio::deferred, ec));

						if (ec == boost::asio::error::operation_aborted) {
							lastTime = self->lastKeepAliveTime;
							continue;
						}
						else if (ec) {
							LOG_ERROR("Timer Error: {}", ec.message().c_str());
							break;
						}

						if (lastTime == self->lastKeepAliveTime) {
							break;
						}
						else {
							lastTime = self->lastKeepAliveTime;
						}
					}

#if !defined(WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL)

					if (self->enableSsl) {
						boost::system::error_code ec;
						co_await self->sslStream.async_shutdown(boost::asio::redirect_error(boost::asio::deferred, ec));
						if (ec) LOG_ERROR("SSL AsyncShutdown Failed: {}", ec.message().c_str());
					}

#endif

					self->closeSocket();

					}, CompletionHandle{});

				LOG_INFO("Keep-Alive Started With Timeout: {} Seconds", timeoutSec.count());

			}

			boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {

				try {

					HttpReadResult readResult = co_await self->asyncRead();

					if (!readResult.succeeded) {

						self->closeSocket();

						co_return;
					}

					self->webrtcSignalManager->getLogicSystem()->postHttpTask(self->shared_from_this(), readResult.httpRequest);

					self->asyncReadKeepAlive(readResult.httpRequest);

				}
				catch (std::exception& e) {

					LOG_ERROR("AsyncRead Or postHttpTask Or AsyncReadKeepAlive Exception: {}", e.what());

					self->closeSocket();

				};

				}, CompletionHandle{});

		}


		boost::asio::awaitable<bool> HttpSocket::asyncWrite(boost::beast::http::response<boost::beast::http::string_body> httpResponse) {

			if (isClosed.load()) co_return false;

			if (isKeepAlive) {

				std::chrono::steady_clock::time_point newExpireTime = std::chrono::steady_clock::now() + timeoutSec;

				lastKeepAliveTime = newExpireTime;

				keepTimer.cancel();

			}

			boost::system::error_code ec;

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			co_await boost::beast::http::async_write(tcpStream, httpResponse, boost::asio::redirect_error(boost::asio::deferred, ec));

			if (ec) {
				LOG_ERROR("Async_write Failed: {}", ec.message().c_str());
				co_return false;
			}

#else

			co_await boost::beast::http::async_write(sslStream, httpResponse, boost::asio::redirect_error(boost::asio::deferred, ec));

			if (ec) {
				LOG_ERROR("SSL AsyncWrite Failed: {}", ec.message().c_str());
				co_return false;
			}

#endif

			co_return true;
		}


		void HttpSocket::closeSocket() {

			if (isClosed.exchange(true)) return;

			keepAliveRunning.store(false);

			keepTimer.cancel();

			boost::system::error_code ec;

#ifdef WEBRTC_SIGNAL_HTTP_SOCKET_DISABLE_SSL

			boost::asio::ip::tcp::socket& socket = tcpStream.socket();

			if (socket.is_open()) {

				socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

				socket.close(ec);

			}

#else

			boost::asio::ip::tcp::socket& socket = sslStream.next_layer();

			if (socket.is_open()) {

				socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

				socket.close(ec);

			}

#endif

		}

		boost::asio::io_context& HttpSocket::getIoContext()
		{
			return ioContext;
		}

		WebrtcSignalManager* HttpSocket::getWebrtcSignalManager()
		{
			return webrtcSignalManager;
		}

		bool HttpSocket::getKeepAlive()
		{
			return isKeepAlive;
		}


	}

}
