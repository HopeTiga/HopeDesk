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

		WebRTCMysqlManager(boost::asio::io_context& ioContext);

		~WebRTCMysqlManager();

		void initConnection(std::string hostIP, size_t port, std::string username, std::string password,std::string database,size_t size = std::thread::hardware_concurrency() * 2);

	private:

		std::shared_ptr<boost::mysql::any_connection> getConnection();

	private:

		boost::asio::io_context& ioContext;

		boost::asio::ssl::context sslContext;

		std::shared_ptr<boost::mysql::any_connection> mysqlConnection;

		std::string hostIP;

		size_t port;

		std::string username;

		std::string password;

		std::string database;


	};
}

