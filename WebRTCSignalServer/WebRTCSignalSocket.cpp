#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalData.h"

#include <boost/uuid/uuid.hpp>           // uuid 类  
#include <boost/uuid/uuid_generators.hpp> // 生成器  
#include <boost/uuid/uuid_io.hpp>  

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
            , channelIndex(channelIndex)
            , webrtcSignalManager(webrtcSignalManager){

            boost::uuids::random_generator gen;

            sessionId = boost::uuids::to_string(gen());

        }

        WebRTCSignalSocket::~WebRTCSignalSocket() {
            clear();
        }

        std::string WebRTCSignalSocket::getSessionId() {
            return sessionId;
		}

        // 关键修改：返回底层 TCP socket（SSL 流里的 next_layer）
        boost::asio::ip::tcp::socket& WebRTCSignalSocket::getSocket() {
            return webSocket.next_layer().next_layer();
        }

        void WebRTCSignalSocket::destroy() {
            bool expected = false;
            if (isDeleted.compare_exchange_strong(expected, true)) {
                this->closeSocket();
            }
        }

        boost::asio::io_context& WebRTCSignalSocket::getIoCompletionPorts() {
            return ioContext;
        }

        boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> & WebRTCSignalSocket::getWebSocket() {
            return webSocket;
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::handShake() {

            boost::beast::flat_buffer buffer;

            boost::beast::http::request<boost::beast::http::string_body> req;

            boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                co_await self->registrationTimeout();
                }, boost::asio::detached);

            try {

                co_await webSocket.next_layer().async_handshake(boost::asio::ssl::stream_base::server, boost::asio::use_awaitable);

                // 1. 读 HTTP 升级请求（从 SSL 流读）
                co_await boost::beast::http::async_read(webSocket.next_layer(), buffer, req, boost::asio::use_awaitable);

                for (auto const& field : req) {
                    LOG_INFO("  %s: %s",
                        std::string(field.name_string()).c_str(),
                        std::string(field.value()).c_str());
                }

                const std::string target(req.target());

                // 3. WebSocket 握手
                co_await webSocket.async_accept(req, boost::asio::use_awaitable);

                setTcpKeepAlive(webSocket.next_layer().next_layer());

                buffer.consume(buffer.size());
            }
            catch (const boost::system::system_error& se) {
                LOG_ERROR("WebRTCSignalServer WebSocket handshake failed! ERROR: %s", se.what());
                destroy();
            }
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
                LOG_WARNING("Rgister Timeout (%d): WebRTCSignalSocket not rigster，close socket.", registrationTimeoutMs);
                destroy();
            }
        }

        void WebRTCSignalSocket::runEventLoop() {

            webSocketRuns.store(true);

            boost::asio::co_spawn(ioContext, reviceCoroutine(), [self = shared_from_this()](std::exception_ptr p) {
                if (p) {
                    try {
                        std::rethrow_exception(p);
                    }
                    catch (const std::runtime_error& e) {
                        if (self->isRegistered && self->onDisConnectHandle) {
                            self->onDisConnectHandle(self->accountId,self->sessionId);
                        }
                        LOG_DEBUG("WebRTCSignalSocket error: %s", e.what());
                    }
                }
            });

            webSocket.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                boost::beast::role_type::server));
        }

        void WebRTCSignalSocket::clear() {
            if (isStop.exchange(true) == false) {
                LOG_INFO("Stop connect...");
                closeSocket();
            }
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::asyncWrite(std::string str)
        {
            // 延长 str 生命周期到整个协程
            co_await webSocket.async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

            co_return;
        }

        void WebRTCSignalSocket::closeSocket() {

            boost::system::error_code ec;

            // 取消底层 TCP socket
            auto& tcpSock = webSocket.next_layer().next_layer();
            tcpSock.cancel(ec);
            if (ec) {
                LOG_ERROR("WebRTCSignalSocket::closeSocket() can't cancel Socket: %s", ec.message().c_str());
            }

            registrationTimer.cancel();

            // WebSocket 关闭帧
            if (webSocket.is_open()) {
                try {
                    webSocket.close(boost::beast::websocket::close_code::normal, ec);
                }
                catch (const std::exception& e) {
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() close WebSocket failed: %s", e.what());
                }
            }

            // SSL 关闭
            if (webSocket.next_layer().next_layer().is_open()) {
                webSocket.next_layer().shutdown(ec);
                webSocket.next_layer().next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                webSocket.next_layer().next_layer().close(ec);
                if (ec && ec != boost::asio::error::not_connected) {
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() close Tcp Socket failed: %s", ec.message().c_str());
                }
            }

			webSocketRuns.store(false);

            LOG_INFO("WebRTCSignalSocket is close");
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::reviceCoroutine() {

            while (webSocketRuns) {

                boost::beast::flat_buffer buffer;

                co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

                std::string dataStr = boost::beast::buffers_to_string(buffer.data());

                buffer.consume(buffer.size());

                boost::json::object json;

                try {
                    json = boost::json::parse(dataStr).as_object();
                }
                catch (const std::exception& e) {
                    LOG_WARNING("WebSocket received invalid JSON: %s", e.what());
                    destroy();
                    co_return;
                }
            
				if (!json.contains("requestType")) throw std::runtime_error("Invalid request: missing requestType");

				if (!this->isRegistered && !json["requestType"].as_int64() == 0 && !json["requestType"].as_int64() == 15 && !json["requestType"].as_int64() == 21) throw std::runtime_error("Not Allow No Register Do Anything");

                auto data = std::make_shared<hope::core::WebRTCSignalData>(std::move(json), shared_from_this(), webrtcSignalManager);

                webrtcSignalManager->getLogicSystem()->postTaskAsync(data);

            }
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

        void WebRTCSignalSocket::setOnDisConnectHandle(std::function<void(std::string,std::string)> handle) {
            this->onDisConnectHandle = handle;
        }

        void WebRTCSignalSocket::setAccountId(const std::string& accountId) { this->accountId = accountId; }

        std::string WebRTCSignalSocket::getAccountId() { return this->accountId; }

        void WebRTCSignalSocket::setRegistered(bool isRegistered) { this->isRegistered = isRegistered; }

        void WebRTCSignalSocket::setLogicSocketType(LogicSocketType type)
        {
            logicSocketType = type;
        }

        LogicSocketType WebRTCSignalSocket::getLogicSocketType()
        {
            return logicSocketType;
        }

        std::string WebRTCSignalSocket::getRemoteAddress()
        {
            return webSocket.next_layer().next_layer().remote_endpoint().address().to_string();
        }

    } // namespace socket
} // namespace hope