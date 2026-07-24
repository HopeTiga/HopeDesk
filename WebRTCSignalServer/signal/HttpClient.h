#pragma once
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace hope {

	namespace signal {
	
		class HttpClient : public std::enable_shared_from_this<HttpClient> {

		public:

			using Request = boost::beast::http::request<boost::beast::http::string_body>;

			using Response = boost::beast::http::response<boost::beast::http::string_body>;

			explicit HttpClient(boost::asio::io_context& ioContext, bool enableSsl = false);

			~HttpClient();

			HttpClient(const HttpClient&) = delete;

			HttpClient& operator=(const HttpClient&) = delete;

			boost::asio::awaitable<Response> asyncRequest(std::string url, Request request);

			void close();

			boost::asio::awaitable<void> connect(const std::string& host, const std::string& port);

		private:

			void closeStream();

			boost::asio::io_context& ioContext;

			boost::asio::ip::tcp::resolver resolver;

			bool enableSsl;

			std::string connectedHost;

			std::string connectedPort;

			std::unique_ptr<boost::beast::tcp_stream> tcpStream;

			std::unique_ptr<boost::beast::ssl_stream<boost::asio::ip::tcp::socket>> sslStream;

			boost::beast::flat_buffer buffer;
		};

	}

}