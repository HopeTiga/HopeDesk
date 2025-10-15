#include "WebRTCSignalSocket.h"


#include "Utils.h"


WebRTCSignalSocket::WebRTCSignalSocket(boost::asio::io_context& ioContext):ioContext(ioContext),writerChannel(ioContext, 1), resolver(ioContext), registrationTimer(ioContext),webSocket(ioContext), pingTimer(ioContext) {
}

WebRTCSignalSocket::~WebRTCSignalSocket() {
    LOG_INFO("WebRTCSignalSocket析构函数触发!");
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
        // 🔍 添加详细日志
        //LOG_INFO("收到握手请求:");
        //LOG_INFO("  Method: %s", std::string(req.method_string()).c_str());
        //LOG_INFO("  Target: %s", std::string(req.target()).c_str());
        //LOG_INFO("  Connection: %s", std::string(req[boost::beast::http::field::connection]).c_str());
        //LOG_INFO("  Upgrade: %s", std::string(req[boost::beast::http::field::upgrade]).c_str());

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

	boost::asio::co_spawn(ioContext, reviceCoroutine(), [](std::exception_ptr p) {});

    boost::asio::co_spawn(ioContext, writerCoroutine(), [](std::exception_ptr p) {});

    boost::asio::co_spawn(ioContext, heartbeatCoroutine(), [](std::exception_ptr p) {});

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
        LOG_ERROR("WebRTCSignalSocket::closeSocket() 无法取消 Socket 操作: %s", ec.message().c_str());
    }

 
    registrationTimer.cancel(); 

    pingTimer.cancel(); // 

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
            LOG_ERROR("WebRTCSignalSocket::closeSocket() 关闭 WebSocket 失败: %s", e.what());
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
            LOG_ERROR("WebRTCSignalSocket::closeSocket() 关闭底层 Socket 失败: %s", ec.message().c_str());
        }
    }

    // 5. 关闭 writerChannel
    writerChannel.close(); // 确保 writerCoroutine 退出等待


    LOG_INFO("WebRTCSignalSocket 连接已关闭和清理。");
}

boost::asio::awaitable<void> WebRTCSignalSocket::reviceCoroutine() {

    while (!isStop) {

        boost::beast::flat_buffer buffer;

        co_await webSocket.async_read(buffer, boost::asio::use_awaitable);

        std::string dataStr = boost::beast::buffers_to_string(buffer.data());

        buffer.consume(buffer.size());

        boost::json::object json = boost::json::parse(dataStr).as_object();

        if (onMessageHandle) {

			onMessageHandle(json , shared_from_this());
        }

    }

}

boost::asio::awaitable<void> WebRTCSignalSocket::writerCoroutine() {

    for (;;) {

        std::string str;

        while (writerQueues.try_dequeue(str)) {
            
			co_await webSocket.async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

        }

        if (!isStop) {

            isSuppendWrite.store(true);

            std::string str;

            while (writerQueues.try_dequeue(str)) {

                co_await webSocket.async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

            }

            co_await writerChannel.async_receive(boost::asio::use_awaitable);

        }
        else {

            std::string str;

            while (writerQueues.try_dequeue(str)) {

                co_await webSocket.async_write(boost::asio::buffer(str), boost::asio::use_awaitable);

            }

            co_return;
        }

    }

    co_return;

}

boost::asio::awaitable<void> WebRTCSignalSocket::heartbeatCoroutine() {

    // 默认行为是自动回复 Pong，无需设置回调。

    try {
        while (!isStop.load()) {

            using namespace std::chrono_literals;

            // 1. 设置下一次 Ping 的等待时间
            pingTimer.expires_after(PING_INTERVAL);

            // 2. 异步等待定时器
            boost::system::error_code ec_wait;
            co_await pingTimer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec_wait));

            if (isStop.load() || ec_wait == boost::asio::error::operation_aborted) {
                // 如果是 stop() 导致的取消，则正常退出
                co_return;
            }

            // 3. 发送 Ping 帧
            // 发送 Ping 是一个异步写操作。如果连接已断开（例如被 NAT 超时），此操作将失败并抛出异常。
            co_await webSocket.async_ping("", boost::asio::use_awaitable);

        }
    }
    catch (const boost::system::system_error& se) {
        if (se.code() != boost::asio::error::operation_aborted) {
            // Ping 失败，说明连接已断开，可能是超时或网络问题。
            LOG_WARNING("WebSocket Ping 失败，连接可能已中断: %s. 触发清理。", se.what());
            this->stop(); // 失败则主动关闭连接
        }
        else {
            // 被 stop() 或析构函数取消
            LOG_DEBUG("心跳协程被取消。");
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("心跳协程出现未预期异常: %s", e.what());
        this->stop();
    }
}

void WebRTCSignalSocket::writerAsync(std::string str) {

    writerQueues.enqueue(str);

    if (isSuppendWrite.exchange(false)) {
        writerChannel.async_send(boost::system::error_code(), [](boost::system::error_code ec) {});
	}

}

void WebRTCSignalSocket::setOnMessageHandle(std::function<void(boost::json::object,std::shared_ptr<WebRTCSignalSocket>)> handle)
{
	this->onMessageHandle = handle;
}
