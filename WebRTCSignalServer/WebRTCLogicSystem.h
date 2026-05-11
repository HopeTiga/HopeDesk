#pragma once

#include <unordered_map>
#include <memory>
#include <functional>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <absl/container/flat_hash_map.h>

#include "WebRTCMysqlManagerPools.h"

namespace hope {

	namespace core {

		class WebRTCSignalData;

		class HttpSocket;

		class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
		{

		public:

			WebRTCLogicSystem(boost::asio::io_context& ioContext, int channelIndex);

			~WebRTCLogicSystem();

			WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

			void operator=(const WebRTCLogicSystem& logic) = delete;

			void postTaskAsync(std::shared_ptr<hope::core::WebRTCSignalData> data);

			void postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest);

			boost::asio::io_context& getIoCompletePorts();

			void initHandlers();

			void initHttpHandlers();

		private:

			boost::asio::io_context& ioContext;

			int channelIndex;

			absl::flat_hash_map<int, std::function<boost::asio::awaitable<void>(std::shared_ptr<hope::core::WebRTCSignalData>)>> webrtcHandlers;

			absl::flat_hash_map<std::string, std::function<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>> httpHandlers;

			std::shared_ptr<hope::mysql::WebRTCMysqlManagerPools> webrtcMysqlManagerPools;
		};
	}

}

