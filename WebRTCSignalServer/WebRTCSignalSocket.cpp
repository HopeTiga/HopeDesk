#include "WebRTCSignalSocket.h"
#include "WebRTCSignalManager.h"
#include "WebRTCLogicSystem.h"
#include "WebRTCSignalData.h"

#include "Utils.h"

namespace hope {
    
    namespace core{
        WebRTCSignalSocket::WebRTCSignalSocket(boost::asio::io_context& ioContext, int channelIndex, WebRTCSignalManager* webrtcSignalManager)
            : ioContext(ioContext)
            , resolver(ioContext)
            , registrationTimer(ioContext)
            , webSocket(ioContext)
            , channelIndex(channelIndex)
            , webrtcSignalManager(webrtcSignalManager) {
        }

        WebRTCSignalSocket::~WebRTCSignalSocket() {

        }

        boost::asio::ip::tcp::socket& WebRTCSignalSocket::getSocket() {

            return webSocket.next_layer();

        }

        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>& WebRTCSignalSocket::getWebSocket() {

            return webSocket;

        }

        boost::asio::awaitable<void> WebRTCSignalSocket::handShake() {
            // 假设 webSocket.next_layer() 已经通过 acceptor.async_accept 连接成功

            boost::beast::flat_buffer buffer;
            boost::beast::http::request<boost::beast::http::string_body> req;

            try {
                // 1. 异步读取 HTTP Upgrade 请求
                co_await boost::beast::http::async_read(webSocket.next_layer(), buffer, req, boost::asio::use_awaitable);

                // 打印所有请求头
                for (auto const& field : req) {
                    LOG_INFO("  %s: %s",
                        std::string(field.name_string()).c_str(),
                        std::string(field.value()).c_str());
                }

                // 2. 打印请求目标 (客户端 handshake("/", ...) 中的路径)
                // req.target() 返回 boost::beast::string_view
                const std::string target(req.target());

                // 3. 执行 WebSocket 服务端握手 (async_accept)
                co_await webSocket.async_accept(req, boost::asio::use_awaitable);
                boost::asio::co_spawn(ioContext, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                    co_await self->registrationTimeout();
                    }, boost::asio::detached);

                buffer.consume(buffer.size());
            }
            catch (const boost::system::system_error& se) {
                LOG_ERROR("服务端 WebSocket 握手失败! 错误: %s", se.what());
                // ... 错误处理 ...
                closeSocket();
            }
        }

        // WebRTCSignalSocket.cpp

        boost::asio::awaitable<void> WebRTCSignalSocket::registrationTimeout() {

            using namespace std::chrono_literals;

            // 1. 设置 10 秒超时
            registrationTimer.expires_after(10s);

            // 2. 异步等待计时器或被取消
            boost::system::error_code ec;

            co_await registrationTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

            // 3. 检查是否超时
            if (ec == boost::asio::error::operation_aborted) {
                // 计时器被取消 (说明在 10s 内完成了注册)
                co_return;
            }

            // 4. 超时发生，且尚未注册，则关闭连接
            if (!isRegistered.load()) {
                LOG_WARNING("注册超时 (10s): 连接未注册，自动关闭 socket.");
                // 调用 stop() 会执行 closeSocket()
                this->stop();
            }
        }

        void WebRTCSignalSocket::start() {

            boost::asio::co_spawn(ioContext, reviceCoroutine(), [self = shared_from_this()](std::exception_ptr p) {

                if (self->isRegistered && self->onDisConnectHandle) {

                    self->onDisConnectHandle(self->accountID);

                }

                });

            webSocket.set_option(boost::beast::websocket::stream_base::timeout::suggested(
                boost::beast::role_type::server));

        }

        void WebRTCSignalSocket::stop() {

            if (isStop.exchange(true) == false) {
                LOG_INFO("正在停止连接...");
                // 确保所有 IO 操作中断
                closeSocket();
            }

        }

        // WebRTCSignalSocket.cpp

        void WebRTCSignalSocket::closeSocket() {

            boost::system::error_code ec;

            webSocket.next_layer().cancel(ec);

            if (ec) {
                LOG_ERROR("WebRTCSignalSocket::closeSocket() can't cancel Socket 操作: %s", ec.message().c_str());
            }

            registrationTimer.cancel();

            // 3. 关闭 WebSocket
            // 发送 WebSocket 关闭帧
            if (webSocket.is_open()) {
                try {
                    // 使用同步 close，因为我们通常在协程外部或清理阶段调用此函数
                    // 协程内部调用 close 通常需要 async_close
                    webSocket.close(boost::beast::websocket::close_code::normal, ec);
                }
                catch (const std::exception& e) {
                    // Beast::close 可能会抛出异常，捕获它
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() close WebSocket failed: %s", e.what());
                }
            }

            // 4. 关闭底层 TCP Socket
            if (webSocket.next_layer().is_open()) {
                webSocket.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
                if (ec && ec != boost::asio::error::not_connected) {
                    // 忽略 not_connected 错误
                }
                webSocket.next_layer().close(ec);
                if (ec) {
                    LOG_ERROR("WebRTCSignalSocket::closeSocket() close Tcp Socket failed: %s", ec.message().c_str());
                }
            }


            LOG_INFO("WebRTCSignalSocket is close");
        }

        boost::asio::awaitable<void> WebRTCSignalSocket::reviceCoroutine() {

            while (!isStop) {

                boost::beast::flat_buffer buffer;

                co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

                std::string dataStr = boost::beast::buffers_to_string(buffer.data());

                buffer.consume(buffer.size());

                boost::json::object json = boost::json::parse(dataStr).as_object();

                std::shared_ptr<WebRTCSignalData> data = std::make_shared<WebRTCSignalData>(std::move(json), shared_from_this(), webrtcSignalManager);

                webrtcSignalManager->getWebRTCLogicSystem()->postMessageToQueue(data);

            }

        }

        void WebRTCSignalSocket::setOnDisConnectHandle(std::function<void(std::string)> handle)
        {
            this->onDisConnectHandle = handle;
        }


        void WebRTCSignalSocket::setAccountID(const std::string& accountID) { this->accountID = accountID; }

        std::string WebRTCSignalSocket::getAccountID() { return  this->accountID; }

        void WebRTCSignalSocket::setTargetID(const std::string& targetID) { this->targetID = targetID; }

        std::string WebRTCSignalSocket::getTargetID(std::string& targetID) { return  this->targetID; }

        void WebRTCSignalSocket::setHashIndex(size_t index) { this->hashIndex = index; }

        size_t WebRTCSignalSocket::getHashIndex(size_t& index) { return  this->hashIndex; }

        void WebRTCSignalSocket::setRegistered(bool isRegistered) { this->isRegistered = isRegistered; }

        void WebRTCSignalSocket::setCloudProcess(bool cloudGame) { this->cloudProcess.store(cloudGame); };

        bool WebRTCSignalSocket::getCloudProcess() { return this->cloudProcess.load(); };

    }

}