#pragma once

#include <unordered_map>
#include <memory>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/any_invocable.h>

#include "WebRTCMysqlManagerPools.h"

#include "AwaitableTask.h"

namespace hope {

	namespace core {

		class WebRTCSignalPacket;

		class HttpSocket;

		class WebRTCLogicSystem : public std::enable_shared_from_this<WebRTCLogicSystem>
		{

		public:

			WebRTCLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues);

			~WebRTCLogicSystem();

			WebRTCLogicSystem(const WebRTCLogicSystem& logic) = delete;

			void operator=(const WebRTCLogicSystem& logic) = delete;

			void postTaskAsync(hope::core::WebRTCSignalPacket packet);

			void postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest);

			boost::asio::io_context& getIoCompletePorts();

			void asyncEvent();

			void closeEvent();

			void asyncTaskExecute();

		private:

			void initHandlers();

			void initHttpHandlers();

		private:

			boost::asio::io_context& ioContext;

			int channelIndex;

			absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<void>(hope::core::WebRTCSignalPacket)>> webrtcHandlers;

			absl::flat_hash_map<std::string, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>> httpHandlers;

			absl::flat_hash_map<int, bool> webrtcLogicHandlers;

			absl::flat_hash_map<std::string, bool> httpLogicHandlers;

			std::shared_ptr<hope::mysql::WebRTCMysqlManagerPools> webrtcMysqlManagerPools;

			std::atomic<size_t> taskQueueSize{ 0 };

			std::atomic<size_t> localTaskQueueSize{ 0 };

			std::atomic<bool> asyncEvents{ false };

			TaskChannel& taskQueues;

			std::atomic<bool> asyncTaskExecutes{ false };

			std::atomic<uint32_t> threshold{ 0 };

			std::atomic<uint32_t> exitThreshold{ 0 };

			std::atomic<uint32_t> asyncThreshold{ 0 };

		};
	}

}

