#include "WebrtcSignalSocket.h"

#include <boost/json.hpp>
#include <boost/url.hpp>
#include <string_view>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "../Ssl.h"

#include "WebrtcSignalManager.h"
#include "WebrtcSignalPacket.h"

#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        WebrtcSignalSocket::WebrtcSignalSocket(boost::asio::io_context& ioContext, WebrtcSignalManager* webrtcSignalManager, int maxTlsHandShakeTime)
            : ioContext(ioContext)
            , resolver(ioContext)
#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)
            , webSocket(ioContext)
#else
            , webSocket(ioContext, getSslContext())
#endif
            , webrtcSignalManager(webrtcSignalManager)
            , asioConcurrentQueue(ioContext.get_executor())
            , handshakeTimeout(maxTlsHandShakeTime) {

            boost::uuids::random_generator gen;

            sessionId = boost::uuids::to_string(gen());

        }

        WebrtcSignalSocket::~WebrtcSignalSocket() {

            closeEvent();

            LOG_INFO("~WebrtcSignalSocket");

        }

        std::string WebrtcSignalSocket::getSessionId() {

            return sessionId;

        }

        boost::asio::ip::tcp::socket& WebrtcSignalSocket::getSocket() {

#if defined(WEBRTC_SIGNAL_SOCKET_DISABLE_SSL)

            return webSocket.next_layer();

#else

            return webSocket.next_layer().next_layer();

#endif

        }

        boost::asio::io_context& WebrtcSignalSocket::getIoCompletionPorts() {

            return ioContext;

        }

        WebrtcSignalSocket::WebSocketStream& WebrtcSignalSocket::getWebSocket() {

            return webSocket;

        }

        boost::asio::awaitable<bool> WebrtcSignalSocket::handShake() {

            boost::beast::flat_buffer buffer;

            boost::beast::http::request<boost::beast::http::string_body> req;

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

                    LOG_WARN("WebrtcSignalSocket Handshake Rejected: Missing Authorization (Expect Authorization Header OR ? Authorization = Query)");

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

                LOG_ERROR("WebrtcSignalSocket handshake failed! ERROR: %s", se.what());

                closeSocket();

                co_return false;

            }

            co_return true;
        }

        void WebrtcSignalSocket::asyncEvent() {

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

                            LOG_ERROR("WebrtcSignalSocket Error: %s", e.what());

                        }
                        catch (...) {

                            if (self->onDisConnectHandle) {

                                self->onDisConnectHandle(self->accountId, self->sessionId);

                            }

                            LOG_ERROR("WebrtcSignalSocket Error: Unknown");

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

        void WebrtcSignalSocket::closeEvent() {

            if (!asyncEvents.exchange(false)) {

                return;

            }

            asioConcurrentQueue.close();

            closeSocket();

        }

        void WebrtcSignalSocket::closeSocket() {

            boost::system::error_code ec;

            boost::asio::ip::tcp::socket& tcpSocket = getSocket();

            if (tcpSocket.is_open()) {

                tcpSocket.cancel(ec);

                if (ec) {
                    LOG_ERROR("WebrtcSignalSocket::closeSocket() cancel failed: %s", ec.message().c_str());
                }

                // Force RST close: skip graceful TCP FIN handshake, send RST immediately
                boost::asio::detail::socket_option::linger<SOL_SOCKET, SO_LINGER> lingerOption(true, 0);

                tcpSocket.set_option(lingerOption, ec);

                if (ec) {
                    LOG_ERROR("WebrtcSignalSocket::closeSocket() set SO_LINGER failed: %s", ec.message().c_str());
                }

                tcpSocket.close(ec);

                if (ec && ec != boost::asio::error::not_connected) {

                    LOG_ERROR("WebrtcSignalSocket::closeSocket() force close failed: %s", ec.message().c_str());

                }

                LOG_INFO("WebrtcSignalSocket is immediately force closed (RST) and resources are freed");

            }

        }

        boost::asio::awaitable<void> WebrtcSignalSocket::reviceCoroutine() {

            boost::beast::flat_buffer buffer;

            while (asyncEvents.load()) {

                co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

                boost::asio::const_buffer cb = buffer.data();

                std::string_view sv(reinterpret_cast<const char*>(cb.data()), cb.size());

                WebrtcSignalPacket webrtcSignalPakcet(shared_from_this(), webrtcSignalManager, webrtcSignalManager->getChannelIndex());

                try {

                    boost::json::value pv = boost::json::parse(sv, webrtcSignalPakcet.request.storage());

                    webrtcSignalPakcet.request = std::move(pv.as_object());

                }
                catch (std::exception& e) {

                    LOG_ERROR("Json Parse Error: %s", e.what());

                    buffer.consume(buffer.size());

					throw std::runtime_error("Json Parse Error");

                }

                buffer.consume(buffer.size());

                if (!webrtcSignalPakcet.request.contains("requestType")) {

                    LOG_ERROR("WebrtcSignalSocket Invalid Request: missing requestType");

                    throw std::runtime_error("Invalid Request: Missing RequestType");

                }

                webrtcSignalPakcet.requestType = webrtcSignalPakcet.request["requestType"].as_int64();

                webrtcSignalManager->getLogicSystem()->postTaskAsync(std::move(webrtcSignalPakcet));

            }
        }

        boost::asio::awaitable<void> WebrtcSignalSocket::writerCoroutine() {

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
                LOG_ERROR("writerCoroutine unhandled exception: %s", e.what());
            }
            catch (...) {
                LOG_ERROR("writerCoroutine unknown exception");
            }
            co_return;
        }

        void WebrtcSignalSocket::setTcpKeepAlive(boost::asio::ip::tcp::socket& sock, int idle, int intvl, int probes)
        {
            int fd = sock.native_handle();
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(__linux__)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes));
#elif defined(_WIN32)
            struct tcp_keepalive kalive {};
            kalive.onoff = 1;
            kalive.keepalivetime = idle * 1000;   // ms
            kalive.keepaliveinterval = intvl * 1000;   // ms
            DWORD bytes_returned = 0;
            WSAIoctl(fd, SIO_KEEPALIVE_VALS,
                &kalive, sizeof(kalive),
                nullptr, 0, &bytes_returned, nullptr, nullptr);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#else
#warning "Unsupported platform, TCP keep-alive parameters not tuned"
#endif
        }

        void WebrtcSignalSocket::asyncWrite(std::string packet) {

            if (!asyncEvents.load()) {

                return;

            }

            asioConcurrentQueue.enqueue(std::move(packet));

        }

        void WebrtcSignalSocket::setOnDisConnectHandle(absl::AnyInvocable<void(std::string, std::string)>&& handle) {
            this->onDisConnectHandle = std::move(handle);
        }

        void WebrtcSignalSocket::setAccountId(const std::string& accountId) { this->accountId = accountId; }

        std::string WebrtcSignalSocket::getAccountId() { return this->accountId; }

        std::string WebrtcSignalSocket::getRemoteAddress()
        {
            return getSocket().remote_endpoint().address().to_string();
        }
    } // namespace socket
} // namespace hope