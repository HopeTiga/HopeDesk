#include "WebRTCSignalSocket.h"

#include <boost/json.hpp>
#include <boost/url.hpp>
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
#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)
            , webSocket(ioContext)
#else
            , webSocket(ioContext, getSslContext())
#endif
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

#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)

            return webSocket.next_layer();

#else

            return webSocket.next_layer().next_layer();

#endif

        }

        boost::asio::io_context& WebRTCSignalSocket::getIoCompletionPorts() {

            return ioContext;

        }
        WebRTCSignalSocket::WebSocketStream& WebRTCSignalSocket::getWebSocket() {

            return webSocket;

        }

        boost::asio::awaitable<bool> WebRTCSignalSocket::handShake() {

            boost::beast::flat_buffer buffer;

            boost::beast::http::request<boost::beast::http::string_body> req;

            static auto handshakeTimeout = std::chrono::milliseconds(ConfigManager::Instance().GetInt("WebRTCSignalServer.socketWaitTime"));

            try {

#if !defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)

                co_await webSocket.next_layer().async_handshake(
                    boost::asio::ssl::stream_base::server,
                    boost::asio::cancel_after(handshakeTimeout, boost::asio::use_awaitable));

#endif

                co_await boost::beast::http::async_read(webSocket.next_layer(), buffer, req,
                    boost::asio::cancel_after(handshakeTimeout, boost::asio::use_awaitable));

                std::string accountId;

                auto authIt = req.find(boost::beast::http::field::authorization);

                if (authIt != req.end()) {

                    accountId = std::string(authIt->value());

                }
                else {

                    auto target = req.target();

                    auto parsed = boost::urls::parse_origin_form(boost::core::string_view(target.data(), target.size()));

                    if (parsed) {

                        auto it = parsed->params().find("authorization");

                        if (it != parsed->params().end()) {

                            auto v = (*it).value;

                            accountId.assign(v.data(), v.size());

                        }

                    }

                }

                if (accountId.empty()) {

                    LOG_WARN("WebRTCSignalSocket Handshake Rejected: Missing Authorization (Expect Authorization Header OR ? Authorization = Query)");

                    closeSocket();

                    co_return false;

                }

                co_await webSocket.async_accept(req, boost::asio::use_awaitable);

                setTcpKeepAlive(getSocket());

                buffer.consume(buffer.size());

                setAccountId(accountId);

                webrtcSignalManager->registerSocket(accountId, shared_from_this());

                LOG_INFO("User Register Successful (HandShake): %s (channelIndex: %d)", accountId.c_str(), webrtcSignalManager->getChannelIndex());

            }
            catch (const boost::system::system_error& se) {

                LOG_ERROR("WebRTCSignalServer WebSocket handshake failed! ERROR: %s", se.what());

                closeSocket();

                co_return false;

            }

            co_return true;
        }

        void WebRTCSignalSocket::asyncEvent() {

            if (asyncEvents.exchange(true)) return;

            boost::asio::co_spawn(ioContext, [self = shared_from_this()]()->boost::asio::awaitable<void> {

                co_await self->reviceCoroutine();

                co_return;

                }, [self = shared_from_this()](std::exception_ptr p) {
                    if (p) {
                        try {

                            std::rethrow_exception(p);

                        }
                        catch (std::exception& e) {

                            if (self->onDisConnectHandle) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("WebRTCSignalSocket error: %s", e.what());

                        }
                        catch (...) {

                            if (self->onDisConnectHandle) {

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

            closeSocket();

        }

        void WebRTCSignalSocket::closeSocket() {

            boost::system::error_code ec;

            auto& tcpSocket = getSocket();

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

                boost::asio::const_buffer cb = buffer.data();

                std::string_view sv(reinterpret_cast<const char*>(cb.data()), cb.size());

                WebRTCSignalPacket webrtcSignalPakcet(shared_from_this(), webrtcSignalManager, webrtcSignalManager->getChannelIndex());

                try {

                    boost::json::value pv = boost::json::parse(sv, webrtcSignalPakcet.request.storage());

                    webrtcSignalPakcet.request = std::move(pv.as_object());

                }
                catch (std::exception& e) {

                    LOG_WARN("JSON Parse Error: %s", e.what());

                    buffer.consume(buffer.size());

                    closeEvent();

                    continue;

                }

                buffer.consume(buffer.size());

                if (!webrtcSignalPakcet.request.contains("requestType")) {

                    LOG_WARN("WebRTCSignalSocket Invalid Request: missing requestType");

                    closeEvent();

                    continue;

                }

                webrtcSignalPakcet.requestType = webrtcSignalPakcet.request["requestType"].as_int64();

                webrtcSignalManager->getLogicSystem()->postTaskAsync(std::move(webrtcSignalPakcet));

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

        void WebRTCSignalSocket::asyncWrite(std::string packet) {

            if (!asyncEvents.load()) {

                return;

            }

            asioConcurrentQueue.enqueue(std::move(packet));

        }

        void WebRTCSignalSocket::setOnDisConnectHandle(absl::AnyInvocable<void(std::string, std::string)>&& handle) {
            this->onDisConnectHandle = std::move(handle);
        }

        void WebRTCSignalSocket::setAccountId(const std::string& accountId) { this->accountId = accountId; }

        std::string WebRTCSignalSocket::getAccountId() { return this->accountId; }

        std::string WebRTCSignalSocket::getRemoteAddress()
        {
            return getSocket().remote_endpoint().address().to_string();
        }
    } // namespace socket
} // namespace hope