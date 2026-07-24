#pragma once
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp> 
#include <boost/beast.hpp>

extern boost::asio::ssl::context sslContext;

extern void initSslContext(std::string certificateFile, std::string privateKeyFile);

extern boost::asio::ssl::context& getSslContext();

