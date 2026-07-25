#include "HttpClient.h"

#include <boost/url.hpp>

#include "../Ssl.h"

namespace hope {

	namespace signal {
	
        HttpClient::HttpClient(boost::asio::io_context& ioContext, bool enableSsl)
            : ioContext(ioContext)
            , enableSsl(enableSsl)
            , resolver(ioContext)
        {
        }

        HttpClient::~HttpClient() {
            close();
        }

        void HttpClient::closeStream() {
            boost::system::error_code ec;
            if (sslStream) {
                sslStream->shutdown(ec);
                sslStream->next_layer().close(ec);
                sslStream.reset();
            }
            if (tcpStream) {
                tcpStream->close();
                tcpStream.reset();
            }
        }

        void HttpClient::close() {
            closeStream();
            connectedHost.clear();
            connectedPort.clear();
        }

        boost::asio::awaitable<void> HttpClient::connect(const std::string& host, const std::string& port) {
            auto results = co_await resolver.async_resolve(host, port, boost::asio::use_awaitable);

            if (enableSsl) {
                if (!sslStream) {
                    sslStream = std::make_unique<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>(
                        ioContext, getSslContext());
                }
                co_await boost::asio::async_connect(sslStream->next_layer(), results, boost::asio::use_awaitable);
                co_await sslStream->async_handshake(boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);
            }
            else {
                if (!tcpStream) {
                    tcpStream = std::make_unique<boost::beast::tcp_stream>(ioContext);
                }
                co_await boost::asio::async_connect(tcpStream->socket(), results, boost::asio::use_awaitable);
            }

            connectedHost = host;
            connectedPort = port;
        }

        boost::asio::awaitable<HttpClient::Response> HttpClient::asyncRequest(std::string url, Request request) {
            // ----- 1. 解析 host 和 port（不依赖 boost::urls）-----
            std::string host;
            std::string port;

            // 如果包含 ':' 且不是 IPv6 地址（IPv6 用 [] 括起来），则按 "host:port" 拆分
            // 简单处理：查找最后一个 ':'，如果前面有 '[' 则认为是 IPv6 地址，不做拆分
            size_t colonPos = url.find_last_of(':');
            if (colonPos != std::string::npos && colonPos != 0) {
                // 检查是否是 IPv6 地址（包含 '[' 或 ']'）
                if (url.find('[') != std::string::npos || url.find(']') != std::string::npos) {
                    // IPv6 地址，不拆分，整体作为 host，port 使用默认
                    host = url;
                    port = enableSsl ? "443" : "80";
                }
                else {
                    // 普通 host:port
                    host = url.substr(0, colonPos);
                    port = url.substr(colonPos + 1);
                }
            }
            else {
                // 没有 port，整体作为 host
                host = url;
                port = enableSsl ? "443" : "80";
            }

            // 若 host 为空则报错
            if (host.empty()) {
                throw std::invalid_argument("Invalid URL: missing host");
            }

            // ----- 2. 确保 request.target() 不为空 -----
            if (request.target().empty()) {
                request.target("/");
            }

            // ----- 3. 自动补 Host 头（如果未设置）-----
            if (!request.count(boost::beast::http::field::host)) {
                request.set(boost::beast::http::field::host, host);
            }

            // ----- 4. 连接复用判断 -----
            bool needConnect;
            if (enableSsl) {
                needConnect = !sslStream || !sslStream->next_layer().is_open()
                    || connectedHost != host || connectedPort != port;
            }
            else {
                needConnect = !tcpStream || !tcpStream->socket().is_open()
                    || connectedHost != host || connectedPort != port;
            }
            if (needConnect) {
                closeStream();
                co_await connect(host, port);
            }

            // ----- 5. 发送请求并读取响应 -----
            try {
                Response response;
                if (enableSsl) {
                    co_await boost::beast::http::async_write(*sslStream, request, boost::asio::use_awaitable);
                    co_await boost::beast::http::async_read(*sslStream, buffer, response, boost::asio::use_awaitable);
                }
                else {
                    co_await boost::beast::http::async_write(*tcpStream, request, boost::asio::use_awaitable);
                    co_await boost::beast::http::async_read(*tcpStream, buffer, response, boost::asio::use_awaitable);
                }

                // ----- 6. 检查 keep-alive -----
                bool keepAlive = (response.version() >= 11);
                if (auto it = response.find(boost::beast::http::field::connection);
                    it != response.end() && it->value() == "close") {
                    keepAlive = false;
                }
                if (!keepAlive) {
                    close();
                }

                co_return std::move(response);
            }
            catch (...) {
                closeStream();
                throw;
            }
        }

	}

}