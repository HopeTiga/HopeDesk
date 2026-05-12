#include "HttpSocket.h"

#include <chrono>

#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"

#include "Utils.h"

namespace hope {

	namespace core {

		HttpSocket::HttpSocket(boost::asio::io_context& ioContext, WebRTCSignalManager* manager, bool enableSsl)
			: ioContext(ioContext)
			, manager(manager)
			, sslStream(nullptr)
			, tcpStream(nullptr)
			, enableSsl(enableSsl)
			, timeoutSec(60)
			, keepTimer(ioContext)
		{

			if (enableSsl) {

				sslStream = std::make_shared<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>(ioContext, WebRTCSignalSocket::getSslContext());

			}
			else {

				tcpStream = std::make_shared<boost::beast::tcp_stream>(ioContext);

			}

			LOG_INFO("HttpSocket");

		}

		HttpSocket::~HttpSocket()
		{
			closeSocket();
			LOG_INFO("~HttpSocket");
		}

		boost::asio::ip::tcp::socket& HttpSocket::getSocket() {

			if (enableSsl) return sslStream->next_layer();
			else return tcpStream->socket();

		}

		boost::asio::awaitable<void> HttpSocket::asyncEventLoop()
		{
			if (!co_await asyncHandShake()) {

				LOG_ERROR("HttpSocket Handshake Failed, Close Socket.");

				if (enableSsl) {

					boost::system::error_code ec;

					co_await sslStream->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

					if (ec) LOG_ERROR("HttpSocket SSL async_shutdown failed: %s", ec.message().c_str());

				}

				closeSocket();

				co_return;
			}

			try {

				boost::beast::http::request<boost::beast::http::string_body> httpRequest = co_await asyncRead();

				manager->getLogicSystem()->postHttpTaskAsync(shared_from_this(), httpRequest);

				asyncReadKeepAlive(httpRequest);

			}
			catch (std::exception& e) {

				LOG_ERROR("HttpSocket asyncRead or postHttpTaskAsync or asyncReadKeepAlive Exception: %s", e.what());

				closeSocket();

			};

			co_return;
		}


		boost::asio::awaitable<bool> HttpSocket::asyncHandShake()
		{
			if (!enableSsl) co_return true;

			auto timeout = std::chrono::seconds(5);

			try {
				boost::system::error_code ec;

				boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
				timer.expires_after(timeout);

				bool isTimeout = false;
				timer.async_wait([&](const boost::system::error_code& error)mutable {
					if (!error) {
						isTimeout = true;
						sslStream->lowest_layer().cancel();
					}
					});

				std::tuple<boost::system::error_code> op = co_await sslStream->async_handshake(
					boost::asio::ssl::stream_base::server,
					boost::asio::as_tuple(boost::asio::use_awaitable)
				);

				timer.cancel();

				auto [handshake_ec] = std::move(op);

				if (isTimeout) {
					LOG_ERROR("HttpSocket::asyncHandShake Timeout after %llds", timeout.count());
					co_return false;
				}

				if (handshake_ec) {
					LOG_ERROR("HttpSocket::asyncHandShake Error: %s", handshake_ec.message().c_str());
					co_return false;
				}

			}
			catch (const std::exception& e) {
				LOG_ERROR("HttpSocket::asyncHandShake Exception: %s", e.what());
				co_return false;
			}

			co_return true;
		}

		boost::asio::awaitable<boost::beast::http::request<boost::beast::http::string_body>> HttpSocket::asyncRead()
		{
			boost::beast::flat_buffer buffer;
			boost::beast::http::request<boost::beast::http::string_body> httpRequest;
			boost::system::error_code ec;

			if (enableSsl) {

				co_await boost::beast::http::async_read(*sslStream, buffer, httpRequest, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
				if (ec) {
					LOG_ERROR("HttpSocket::asyncRead SSL async_read failed: %s", ec.message().c_str());
					throw std::runtime_error("HttpSocket SSL async_read failed");
				}
			}
			else {
				co_await boost::beast::http::async_read(*tcpStream, buffer, httpRequest, boost::asio::redirect_error(boost::asio::use_awaitable, ec));

				if (ec) {
					LOG_ERROR("HttpSocket::asyncRead async_read failed: %s", ec.message().c_str());
					throw std::runtime_error("HttpSocket async_read failed");
				}
			}

			co_return httpRequest;
		}

		void HttpSocket::asyncReadKeepAlive(boost::beast::http::request<boost::beast::http::string_body>& httpRequest)
		{

			isKeepAlive = httpRequest.keep_alive();

			if (!isKeepAlive) {

				keepTimer.cancel();

				return;
			}

			auto it = httpRequest.find("Keep-Alive");
			if (it != httpRequest.end()) {
				std::string value = it->value().data();
				size_t pos = value.find("timeout=");
				if (pos != std::string::npos) {
					pos += 8; // strlen("timeout=")
					size_t end = value.find(',', pos);
					try {
						int sec = std::stoi(value.substr(pos, end - pos));
						if (sec > 0) {
							timeoutSec = std::chrono::seconds(sec);
						}
					}
					catch (...) {
						LOG_WARN("Failed to parse Keep-Alive timeout value: %s", value.c_str());
					}
				}
			}

			auto newExpireTime = std::chrono::steady_clock::now() + timeoutSec;

			lastKeepAliveTime = newExpireTime;

			if (!keepAliveRunning.exchange(true)) {

				boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {

					auto lastTime = self->lastKeepAliveTime;

					while (self->isKeepAlive) {
						self->keepTimer.expires_at(lastTime);  // 等待到绝对时间点
						boost::system::error_code ec;
						co_await self->keepTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

						if (ec == boost::asio::error::operation_aborted) {
							lastTime = self->lastKeepAliveTime;
							continue;
						}
						else if (ec) {
							LOG_ERROR("HttpSocket::KeepAlive timer error: %s", ec.message().c_str());
							break;
						}

						if (lastTime == self->lastKeepAliveTime) {
							break;
						}
						else {
							lastTime = self->lastKeepAliveTime;
						}
					}

					if (self->enableSsl) {
						boost::system::error_code ec;
						co_await self->sslStream->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
						if (ec) LOG_ERROR("HttpSocket SSL async_shutdown failed: %s", ec.message().c_str());
					}

					self->closeSocket();

					}, boost::asio::detached);

				LOG_INFO("HttpSocket Keep-Alive Started with timeout: %lld seconds", timeoutSec.count());

			}

			boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {

				try {

					boost::beast::http::request<boost::beast::http::string_body> httpRequest = co_await self->asyncRead();

					self->manager->getLogicSystem()->postHttpTaskAsync(self->shared_from_this(), httpRequest);

					self->asyncReadKeepAlive(httpRequest);

				}
				catch (std::exception& e) {

					LOG_ERROR("HttpSocket asyncRead or postHttpTaskAsync or asyncReadKeepAlive Exception: %s", e.what());

					self->closeSocket();

				};

				}, boost::asio::detached);

		}


		boost::asio::awaitable<bool> HttpSocket::asyncWrite(boost::beast::http::response<boost::beast::http::string_body> httpResponse) {

			if (isClosed.load()) co_return false;

			if (isKeepAlive) {

				auto newExpireTime = std::chrono::steady_clock::now() + timeoutSec;

				lastKeepAliveTime = newExpireTime;

				keepTimer.cancel();

			}

			boost::system::error_code ec;
			if (enableSsl) {

				co_await boost::beast::http::async_write(*sslStream, httpResponse, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
				if (ec) {
					LOG_ERROR("HttpSocket::asyncWrite SSL async_write failed: %s", ec.message().c_str());
					co_return false;
				}
			}
			else {

				co_await boost::beast::http::async_write(*tcpStream, httpResponse, boost::asio::redirect_error(boost::asio::use_awaitable, ec));
				if (ec) {
					LOG_ERROR("HttpSocket::asyncWrite async_write failed: %s", ec.message().c_str());
					co_return false;
				}
			}
			co_return true;
		}


		void HttpSocket::closeSocket() {

			if (isClosed.exchange(true)) return;

			keepAliveRunning.store(false);

			keepTimer.cancel();

			boost::system::error_code ec;

			if (enableSsl && sslStream) {

				auto& socket = sslStream->lowest_layer();
				if (socket.is_open()) {
					socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					socket.close(ec);
				}
			}
			else if (tcpStream) {

				auto& socket = tcpStream->socket();
				if (socket.is_open()) {
					socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					socket.close(ec);
				}
			}

		}

		boost::asio::io_context& HttpSocket::getIoContext()
		{
			return ioContext;
		}

		WebRTCSignalManager* HttpSocket::getWebRTCSignalManager()
		{
			return manager;
		}

		bool HttpSocket::getKeepAlive()
		{
			return isKeepAlive;
		}


	}

}