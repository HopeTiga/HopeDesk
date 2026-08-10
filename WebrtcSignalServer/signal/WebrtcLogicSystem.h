#pragma once

#include <unordered_map>
#include <memory>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/any_invocable.h>

#include "../mysql/WebrtcMysqlManagerPools.h"

#include "AwaitableTask.h"

#include "HttpFilters.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

		class WebrtcSignalPacket;

		class HttpSocket;

		class WebrtcLogicSystem : public std::enable_shared_from_this<WebrtcLogicSystem>
		{

		public:

			WebrtcLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues, int threshold, int exitThreshold, int asyncThreshold);

			~WebrtcLogicSystem();

			WebrtcLogicSystem(const WebrtcLogicSystem& logic) = delete;

			void operator=(const WebrtcLogicSystem& logic) = delete;

			void postTaskAsync(hope::signal::WebrtcSignalPacket packet);

			void postHttpTaskAsync(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest);

			boost::asio::io_context& getIoCompletionPorts();

			void asyncEvent();

			void closeEvent();

			void asyncTaskExecute();

		private:

			void initHandlers();

			void initFilters();

			void initHttpHandlers();

		public:

			boost::asio::io_context& ioContext;

			int channelIndex;

			absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<void>(hope::signal::WebrtcSignalPacket)>> webrtcHandlers;

			absl::flat_hash_map<std::string, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>> httpHandlers;

			absl::flat_hash_map<int, bool> webrtcLogicHandlers;

			absl::flat_hash_map<std::string, bool> httpLogicHandlers;

			HttpFilters httpFilters;

			std::shared_ptr<hope::mysql::WebrtcMysqlManagerPools> webrtcMysqlManagerPools;

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

