#pragma once
#include <memory>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/json.hpp>

#include "concurrentqueue.h"

class WebRTCSignalServer;

class WebRTCSignalSocket : public std::enable_shared_from_this<WebRTCSignalSocket>
{
public:

	WebRTCSignalSocket(boost::asio::io_context& ioContext,int channelIndex,WebRTCSignalServer & webrtcSignalServer);

	~WebRTCSignalSocket();

	boost::asio::ip::tcp::socket& getSocket();

	boost::beast::websocket::stream<boost::asio::ip::tcp::socket>& getWebSocket();

	boost::asio::awaitable<void> handShake();

	void start();

	void stop();

	boost::asio::awaitable<void> reviceCoroutine();

	boost::asio::awaitable<void> writerCoroutine();

	void writerAsync(std::string str);

	void setAccountID(const std::string& accountID) { this->accountID = accountID; }

	std::string& getAccountID() { return accountID = this->accountID; }

	void setTargetID(const std::string& targetID) { this->targetID = targetID; }

	std::string& getTargetID(std::string& targetID) { return  targetID = this->targetID; }

	void setHashIndex(size_t index) { this->hashIndex = index; }

	size_t& getHashIndex(size_t& index) { return  index = this->hashIndex; }

	void setRegistered(bool isRegistered) { this->isRegistered = isRegistered; }
	

public:

	void setOnMessageHandle(std::function<void(boost::json::object,std::shared_ptr<WebRTCSignalSocket>)> handle);

private:

	void closeSocket();

	boost::asio::awaitable<void> registrationTimeout();

private:

	WebRTCSignalServer& webrtcSignalServer;

	boost::asio::io_context& ioContext;

	boost::beast::websocket::stream<boost::asio::ip::tcp::socket> webSocket;

	boost::asio::ip::tcp::resolver resolver;

	moodycamel::ConcurrentQueue<std::string> writerQueues;

	boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> writerChannel;

	std::atomic<bool> isStop{ false };

	std::string accountID;

	std::string targetID;

	size_t hashIndex{ 0 };

	std::atomic<bool> isSuppendWrite{ false };

	boost::asio::steady_timer registrationTimer; // 计时器成员

	std::atomic<bool> isRegistered{ false }; // 新增：注册状态标志

	int channelIndex;

private:

	std::function<void(boost::json::object, std::shared_ptr<WebRTCSignalSocket>)> onMessageHandle;
};

