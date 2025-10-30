#pragma once
#include <memory>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/json.hpp>

#include "concurrentqueue.h"


namespace hope {

	namespace core {
		
		class WebRTCSignalManager;

		class WebRTCSignalSocket : public std::enable_shared_from_this<WebRTCSignalSocket>
		{
		public:

			WebRTCSignalSocket(boost::asio::io_context& ioContext, int channelIndex, WebRTCSignalManager* webrtcSignalManager);

			~WebRTCSignalSocket();

			boost::asio::ip::tcp::socket& getSocket();

			boost::beast::websocket::stream<boost::asio::ip::tcp::socket>& getWebSocket();

			boost::asio::awaitable<void> handShake();

			void start();

			void stop();

			boost::asio::awaitable<void> reviceCoroutine();

			void setAccountID(const std::string& accountID);

			std::string getAccountID();

			void setTargetID(const std::string& targetID);

			std::string getTargetID(std::string& targetID);

			void setHashIndex(size_t index);

			size_t getHashIndex(size_t& index);

			void setRegistered(bool isRegistered);

			void setCloudProcess(bool cloudProcess);

			bool getCloudProcess();
		public:

			void setOnDisConnectHandle(std::function<void(std::string)> handle);

		private:

			void closeSocket();

			boost::asio::awaitable<void> registrationTimeout();

		private:

			WebRTCSignalManager* webrtcSignalManager;

			boost::asio::io_context& ioContext;

			boost::beast::websocket::stream<boost::asio::ip::tcp::socket> webSocket;

			boost::asio::ip::tcp::resolver resolver;

			std::atomic<bool> isStop{ false };

			std::string accountID;

			std::string targetID;

			size_t hashIndex{ 0 };

			std::atomic<bool> isSuppendWrite{ false };

			boost::asio::steady_timer registrationTimer; // 计时器成员

			std::atomic<bool> isRegistered{ false }; // 新增：注册状态标志

			int channelIndex;

			std::atomic<bool> cloudProcess{ false };

		private:

			std::function<void(std::string)> onDisConnectHandle;
		};

	}

}
