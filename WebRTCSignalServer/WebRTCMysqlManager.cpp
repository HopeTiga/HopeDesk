#include "WebRTCMysqlManager.h"
#include "AsioProactors.h"
#include "Logger.h"
#include <iostream>


namespace hope {

	namespace core {
		WebRTCMysqlManager::WebRTCMysqlManager(boost::asio::io_context& ioContext) : sslContext(boost::asio::ssl::context::tls_client), ioContext(ioContext)
		{

		}
		WebRTCMysqlManager::~WebRTCMysqlManager()
		{
		}
		void WebRTCMysqlManager::initConnection(std::string hostIP, size_t port, std::string username, std::string password, std::string database, size_t size)
		{

			this->hostIP = hostIP;

			this->port = port;

			this->username = username;

			this->password = password;

			this->database = database;

			boost::mysql::connect_params  params;

			params.server_address = boost::mysql::host_and_port(hostIP, static_cast<unsigned short>(port));

			params.username = username;

			params.password = password;

			params.database = database;

			params.ssl = boost::mysql::ssl_mode::disable;

			mysqlConnection = std::make_shared<boost::mysql::any_connection>(ioContext);

			boost::asio::co_spawn(ioContext, [this, params, hostIP]() -> boost::asio::awaitable<void> {

				try {

					co_await mysqlConnection->async_connect(params);

				}
				catch (const std::exception& e) {
					Logger::getInstance()->error(std::string("MySQL Connection failed: ") + e.what());
				}

				// 连接成功，可以执行查询等操作
				}, boost::asio::detached);

		}

		std::shared_ptr<boost::mysql::any_connection> WebRTCMysqlManager::getConnection()
		{

			return mysqlConnection;

		}
	}

}
