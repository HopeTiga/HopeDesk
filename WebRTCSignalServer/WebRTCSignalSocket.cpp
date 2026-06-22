#include "WebRTCSignalSocket.h"

#include <boost/json.hpp>
#include <string_view>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>  

#include "WebRTCSignalManager.h"
#include "WebRTCSignalPacket.h"

#include "ConfigManager.h"
#include "Utils.h"

namespace hope {

    namespace core {

        boost::asio::ssl::context WebRTCSignalSocket::sslContext{ boost::asio::ssl::context::tlsv12 };

        void WebRTCSignalSocket::initSslContext()
        {

            sslContext.set_options(
                boost::asio::ssl::context::default_workarounds |
                boost::asio::ssl::context::no_sslv2 |
                boost::asio::ssl::context::single_dh_use);

            sslContext.use_certificate_chain_file(ConfigManager::Instance().GetString("WebRTCSignalServer.certificateFile"));


            sslContext.use_private_key_file(ConfigManager::Instance().GetString("WebRTCSignalServer.privateKeyFile"), boost::asio::ssl::context::pem);

        }

        boost::asio::ssl::context& WebRTCSignalSocket::getSslContext()
        {

            return sslContext;

        }

        WebRTCSignalSocket::WebRTCSignalSocket(boost::asio::io_context& ioContext, WebRTCSignalManager* webrtcSignalManager)
            : ioContext(ioContext)
            , resolver(ioContext)
            , registrationTimer(ioContext)
            , webSocket(ioContext, getSslContext())
            , webrtcSignalManager(webrtcSignalManager)
            , asioConcurrentQueue(ioContext.get_executor()) {

            boost::uuids::random_generator gen;

            sessionId = boost::uuids::to_string(gen());

        }

        WebRTCSignalSocket::~WebRTCSignalSocket() {

            closeEvent();

            LOG_INFO("~WebRTCSignalSocket");

        }

        std::string WebRTCSignalSocket::getSessionId() {

            return sessionId;

        }

        boost::asio::ip::tcp::socket& WebRTCSignalSocket::getSocket() {

            return webSocket.next_layer().next_layer();

        }

        boost::asio::io_context& WebRTCSignalSocket::getIoCompletionPorts() {

            return ioContext;

        }
        boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>& WebRTCSignalSocket::getWebSocket() {

            return webSocket;

        }

        boost::asio::awaitable<bool> WebRTCSignalSocket::handShake() {

            boost::beast::flat_buffer buffer;

            boost::beast::http::request<boost::beast::http::string_body> req;

            try {

                co_await webSocket.next_layer().async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);

                co_await boost::beast::http::async_read(webSocket.next_layer(), buffer, req, boost::asio::use_awaitable);

                co_await webSocket.async_accept(req, boost::asio::use_awaitable);

                setTcpKeepAlive(webSocket.next_layer().next_layer());

                buffer.consume(buffer.size());

            }
            catch (const boost::system::system_error& se) {

                LOG_ERROR("WebRTCSignalServer WebSocket handshake failed! ERROR: %s", se.what());

                closeSocket();

                co_return false;

            }

            co_return true;
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::registrationTimeout() {

            static int registrationTimeoutMs = ConfigManager::Instance().GetInt("WebRTCSignalServer.socketWaitTime");

            registrationTimer.expires_after(std::chrono::milliseconds(registrationTimeoutMs));

            boost::system::error_code ec;

            co_await registrationTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            if (ec == boost::asio::error::operation_aborted) {

                co_return;

            }
            if (!isRegistered.load()) {

                LOG_WARN("Rgister Timeout (%d): WebRTCSignalSocket not register,close socket.", registrationTimeoutMs);

                closeEvent();

            }

            co_return;

        }

        void WebRTCSignalSocket::asyncEvent() {

            if (asyncEvents.exchange(true)) return;

            boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {

                co_await self->registrationTimeout();

                }, boost::asio::detached);

            boost::asio::co_spawn(ioContext, [self = shared_from_this()]()->boost::asio::awaitable<void> {

                co_await self->reviceCoroutine();

                co_return;

                }, [self = shared_from_this()](std::exception_ptr p) {
                    if (p) {
                        try {

                            std::rethrow_exception(p);

                        }
                        catch (std::exception& e) {

                            if (self->isRegistered && self->onDisConnectHandle) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("WebRTCSignalSocket error: %s", e.what());

                        }
                        catch (...) {

                            if (self->isRegistered && self->onDisConnectHandle) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("WebRTCSignalSocket error: unknown");

                        }
                    }
                    });

                boost::asio::co_spawn(ioContext, [self = shared_from_this()]()->boost::asio::awaitable<void> {

                    co_await self->writerCoroutine();

                    co_return;

                    }, boost::asio::detached);

                webSocket.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                    boost::beast::role_type::server));

        }

        void WebRTCSignalSocket::closeEvent() {

            if (!asyncEvents.exchange(false)) {

                return;

            }

            asioConcurrentQueue.close();

            registrationTimer.cancel();

            closeSocket();

        }

        void WebRTCSignalSocket::closeSocket() {

            boost::system::error_code ec;

            auto& tcpSocket = webSocket.next_layer().next_layer();

            if (tcpSocket.is_open()) {

                tcpSocket.cancel(ec);

                if (ec) {
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() cancel failed: %s", ec.message().c_str());
                }

                tcpSocket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);

                tcpSocket.close(ec);

                if (ec && ec != boost::asio::error::not_connected) {
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() force close failed: %s", ec.message().c_str());
                }

                LOG_INFO("WebRTCSignalSocket is immediately closed and resources are freed");

            }

        }

        boost::asio::awaitable<void> WebRTCSignalSocket::reviceCoroutine() {

            boost::beast::flat_buffer buffer;

            while (asyncEvents.load()) {

                co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

                // flat_buffer::data() is a single contiguous const_buffer — build a
                // string_view over it directly, no buffers_to_string copy.
                boost::asio::const_buffer cb = buffer.data();

                std::string_view sv(reinterpret_cast<const char*>(cb.data()), cb.size());

                std::shared_ptr<WebRTCSignalPacket> packet = std::make_shared<WebRTCSignalPacket>(shared_from_this(), webrtcSignalManager, webrtcSignalManager->getChannelIndex());

                try {

                    boost::json::value pv = boost::json::parse(sv, packet->request.storage());

                    packet->request = std::move(pv.as_object());

                }
                catch (std::exception& e) {

                    LOG_WARN("JSON Parse Error: %s", e.what());

                    buffer.consume(buffer.size());

                    closeEvent();

                    continue;

                }

                buffer.consume(buffer.size());

                if (!packet->request.contains("requestType")) {

                    LOG_WARN("WebRTCSignalSocket Invalid Request: missing requestType");

                    closeEvent();

                    continue;

                }

                if (!this->isRegistered && packet->request["requestType"].as_int64() != 0) {

                    LOG_ERROR("WebRTCSignalSocket Not Registered, RequestType: %d", packet->request["requestType"].as_int64());

                    closeEvent();

                    continue;

                }

                webrtcSignalManager->getLogicSystem()->postTaskAsync(std::move(packet));

            }
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::writerCoroutine() {

            try {

                while (asyncEvents.load()) {

                    std::optional<std::string> optional = co_await asioConcurrentQueue.dequeue();

                    if (optional.has_value()) {

                        std::string packet = std::move(optional.value());

                        co_await webSocket.async_write(boost::asio::buffer(packet), boost::asio::use_awaitable);

                    }
                    else break;

                    if (!asyncEvents.load()) break;

                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Writer coroutine unhandled exception: %s", e.what());
            }
            catch (...) {
                LOG_ERROR("Writer coroutine unknown exception");
            }
            co_return;
        }

        void WebRTCSignalSocket::setTcpKeepAlive(boost::asio::ip::tcp::socket& sock, int idle, int intvl, int probes)
        {
            int fd = sock.native_handle();
            /* 1. 先打开 SO_KEEPALIVE 通用开关 */
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(__linux__)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
#elif defined(_WIN32)
            /* Windows 用毫秒结构体 */
            struct tcp_keepalive kalive {};
            kalive.onoff = 1;
            kalive.keepalivetime = idle * 1000;   // ms
            kalive.keepaliveinterval = intvl * 1000;   // ms
            DWORD bytes_returned = 0;
            WSAIoctl(fd, SIO_KEEPALIVE_VALS,
                &kalive, sizeof(kalive),
                nullptr, 0, &bytes_returned, nullptr, nullptr);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
            /* macOS / BSD 用秒级 TCP_KEEPALIVE 等选项 */
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));   // 同 Linux 的 IDLE
            /* 间隔与次数在 BSD 上只有一个 TCP_KEEPINTVL，单位秒 */
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            /* BSD 没有 KEEPCNT，用 TCP_KEEPALIVE 的初始值+间隔推算，效果相近 */
#else
#warning "Unsupported platform, TCP keep-alive parameters not tuned"
#endif
        }

        void WebRTCSignalSocket::asyncWrite(unsigned char* packet, size_t size) {

            asyncWrite(std::string(reinterpret_cast<const char*>(packet), size));

            if (packet) {

                delete[] packet;

                packet = nullptr;

            }

        }

        void WebRTCSignalSocket::asyncWrite(std::string packet) {

            if (!asyncEvents.load()) return;

            asioConcurrentQueue.enqueue(std::move(packet));

        }

        void WebRTCSignalSocket::setOnDisConnectHandle(absl::AnyInvocable<void(std::string, std::string)>&& handle) {
            this->onDisConnectHandle = std::move(handle);
        }

        void WebRTCSignalSocket::setAccountId(const std::string& accountId) { this->accountId = accountId; }

        std::string WebRTCSignalSocket::getAccountId() { return this->accountId; }

        void WebRTCSignalSocket::setRegistered(bool isRegistered) { this->isRegistered = isRegistered; }

        bool WebRTCSignalSocket::getRegistered() { return this->isRegistered; }

        std::string WebRTCSignalSocket::getRemoteAddress()
        {
            return webSocket.next_layer().next_layer().remote_endpoint().address().to_string();
        }
    } // namespace socket
} // namespace hope