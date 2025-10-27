#include "WebRTCMysqlManager.h"
#include "AsioProactors.h"
#include "Logger.h"


namespace Hope {

	WebRTCMysqlManager::WebRTCMysqlManager():mysqlConnections(0), sslContext(boost::asio::ssl::context::tls_client)
	{
	}
	WebRTCMysqlManager::~WebRTCMysqlManager()
	{
	}
	void WebRTCMysqlManager::initConnection(std::string hostIP,size_t port, std::string username, std::string password,std::string database ,size_t size)
	{

		this->hostIP = hostIP;

		this->port = port;

		this->username = username;

		this->password = password;

		this->database = database;

		this->size = size;

		boost::mysql::handshake_params params(
			username, // 数据库用户名
			password, // 数据库密码
			database // 数据库名
		);

		mysqlConnections.resize(size);

		for (int i = 0; i < size; i++) {

			std::pair<int, boost::asio::io_context& > pairs = AsioProactors::getInstance()->getIoCompletePorts();

			mysqlConnections[i] = std::move(std::make_shared<boost::mysql::tcp_ssl_connection>(pairs.second, sslContext));

			boost::asio::co_spawn(pairs.second, [this, i, params, hostIP]() -> boost::asio::awaitable<void> {

				try {

					boost::asio::ip::tcp::resolver resolver(co_await boost::asio::this_coro::executor);

					auto endpoints = co_await resolver.async_resolve(
						hostIP, "3306", boost::asio::use_awaitable
					);

					co_await mysqlConnections[i]->async_connect(
						*endpoints.begin(),
						params,
						boost::asio::use_awaitable
					);

				}
				catch (const std::exception& e) {
					Logger::getInstance()->error(std::string("MySQL Connection failed: ") + e.what());
				}

				// 连接成功，可以执行查询等操作
				}, boost::asio::detached);

		}

	}

	std::shared_ptr<boost::mysql::tcp_ssl_connection> WebRTCMysqlManager::getConnection()
	{

		size_t index = currentIndex.fetch_add(1) % size;

		return mysqlConnections[index];

	}

}
