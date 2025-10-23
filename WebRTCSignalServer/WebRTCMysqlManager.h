#pragma once
#include <memory>
#include <vector>
#include <thread>
#include <string>

#include <boost/asio.hpp>
#include <boost/mysql.hpp>


namespace Hope {

	class WebRTCMysqlManager : public std::enable_shared_from_this<WebRTCMysqlManager>
	{

	public:

		WebRTCMysqlManager();

		~WebRTCMysqlManager();

		static std::shared_ptr<WebRTCMysqlManager> getInstance() {

			static std::shared_ptr<WebRTCMysqlManager> instance = std::make_shared<WebRTCMysqlManager>();

			return instance;

		}

		void initConnection(std::string hostIP, size_t port, std::string username, std::string password,std::string database,size_t size = std::thread::hardware_concurrency() * 2);

	private:

		std::shared_ptr<boost::mysql::tcp_ssl_connection> getConnection();

	private:

		boost::asio::ssl::context sslContext;

		std::vector<std::shared_ptr<boost::mysql::tcp_ssl_connection>> mysqlConnections;

		std::string hostIP;

		size_t port;

		std::string username;

		std::string password;

		std::string database;

		size_t size;

		std::atomic<size_t> currentIndex{ 0 };

	};
}

