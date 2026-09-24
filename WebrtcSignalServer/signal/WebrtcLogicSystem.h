#pragma once


#include <unordered_map>
#include <memory>
#include <utility>
#include <exception>
#include <stdexcept>
#include <string_view>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <ylt/struct_pack.hpp>

#include "../utils/StringHasher.h"
#include <absl/functional/any_invocable.h>
#include <absl/strings/str_format.h>

#include "WebrtcSignalSocket.h"
#include "WebrtcSignalPacket.h"

#include "HttpFilters.h"

#include "AwaitableTask.h"

#include "../storage/MysqlConfig.h"
#include "../storage/RedisConfig.h"
#include "../storage/MysqlManagerPools.h"
#include "../storage/RedisWrapper.h"

#include "../utils/CompletionHandle.h"
#include "../utils/Utils.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

		class HttpSocket;

		using WebrtcHandler = absl::AnyInvocable<boost::asio::awaitable<void>(hope::signal::WebrtcSignalPacket)>;

		using WebrtcValueHandler = absl::AnyInvocable<boost::asio::awaitable<boost::json::value>(hope::signal::WebrtcSignalPacket)>;

		using HttpHandler = absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>;

		struct PostedTask {

			std::shared_ptr<WebrtcSignalManager>* webrtcSignalManager = nullptr;

			absl::AnyInvocable<void(std::shared_ptr<WebrtcSignalManager>&)> asyncHandle;

		};

		struct WebrtcLogicConfig {

			int threshold = 256;          // 全局任务队列高水位

			int exitThreshold = 128;      // 本地队列高水位,触发切本地处理

			int asyncThreshold = 32;      // 异步派发阈值

			hope::storage::MysqlConfig mysqlConfig;

			hope::storage::RedisConfig redisConfig;

		};

		class WebrtcLogicSystem : public std::enable_shared_from_this<WebrtcLogicSystem>
		{

		public:

			static constexpr std::size_t maximumTasksPerExecute = 8;

			WebrtcLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues, WebrtcLogicConfig webrtcLogicConfig);

			~WebrtcLogicSystem();

			WebrtcLogicSystem(const WebrtcLogicSystem& logic) = delete;

			void operator=(const WebrtcLogicSystem& logic) = delete;

			void postTask(hope::signal::WebrtcSignalPacket webrtcSignalPacket);

			bool postTask(PostedTask task);

			template <typename CompletionToken = CompletionHandle>
			typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr)>::return_type
			postTask(hope::signal::WebrtcSignalPacket webrtcSignalPacket, CompletionToken&& token)
			{

				using CompletionHandlerType = typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr)>::completion_handler_type;

				int type = webrtcSignalPacket.webrtcEnvelope.requestType;

				return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr)>(
					[this, type, webrtcSignalPacket = std::move(webrtcSignalPacket)](CompletionHandlerType completionHandler) mutable {

						std::shared_ptr<CompletionHandlerType> completionHandlerPtr = std::make_shared<CompletionHandlerType>(std::move(completionHandler));

						boost::unordered_flat_map<int, std::unique_ptr<WebrtcHandler>>::iterator iterator = this->webrtcHandlers.find(type);

						if (iterator != this->webrtcHandlers.end()) {

							WebrtcHandler* func = iterator->second.get();

							boost::asio::co_spawn(ioContext, [type, func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<void> {

								co_await(*func)(std::move(webrtcSignalPacket));

								},
								[this, completionHandlerPtr](std::exception_ptr exception) mutable {

									(*completionHandlerPtr)(exception);

								});

						}
						else {

							LOG_ERROR("Unknown Webrtc Request Type: {}", type);

							boost::asio::post(boost::asio::get_associated_executor(*completionHandlerPtr, ioContext), [completionHandlerPtr]() mutable {

								(*completionHandlerPtr)(std::exception_ptr{});

								});

						}
					},
					token);

			}

			template <typename CompletionToken = CompletionHandle>
			typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr, boost::json::value)>::return_type
			coPostTask(hope::signal::WebrtcSignalPacket webrtcSignalPacket, CompletionToken&& token = CompletionToken{})
			{

				using CompletionHandlerType = typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr, boost::json::value)>::completion_handler_type;

				int type = webrtcSignalPacket.webrtcEnvelope.requestType;

				return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr, boost::json::value)>(
					[this, type, webrtcSignalPacket = std::move(webrtcSignalPacket)](CompletionHandlerType completionHandler) mutable {

						std::shared_ptr<CompletionHandlerType> completionHandlerPtr = std::make_shared<CompletionHandlerType>(std::move(completionHandler));

						boost::unordered_flat_map<int, std::unique_ptr<WebrtcValueHandler>>::iterator iterator = this->webrtcValueHandlers.find(type);

						if (iterator != this->webrtcValueHandlers.end()) {

							WebrtcValueHandler* func = iterator->second.get();

							boost::asio::co_spawn(ioContext, [type, func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<boost::json::value> {

								co_return co_await(*func)(std::move(webrtcSignalPacket));

								},
								[this, completionHandlerPtr](std::exception_ptr exception, boost::json::value value = {}) mutable {

									(*completionHandlerPtr)(std::move(exception), std::move(value));

								});

						}
						else {

							LOG_ERROR("Unknown Webrtc Request Type: {}", type);

							boost::asio::post(boost::asio::get_associated_executor(*completionHandlerPtr, ioContext), [completionHandlerPtr, type]() mutable {

								(*completionHandlerPtr)(std::make_exception_ptr(std::runtime_error(absl::StrFormat("Unknown Webrtc Request Type: %d", type))), boost::json::value{});

								});

						}
					},
					token);

			}

			void postHttpTask(std::shared_ptr<HttpSocket> httpSocket, boost::beast::http::request<boost::beast::http::string_body> httpRequest);

			boost::asio::io_context& getIoCompletionPorts();

			void asyncEvent();

			void closeEvent();

			void closeExecute();

			void asyncTaskExecute();

		private:

			void asyncExecute();

			void initHandlers();

			void initFilters();

			void initHttpHandlers();

			hope::storage::RedisWrapper& loadRedisWrapper();

			hope::storage::RedisWrapper& loadRedisWrapper(std::string_view key);

		private:

			boost::asio::io_context& ioContext;

			int channelIndex;

			boost::unordered_flat_map<int, std::unique_ptr<WebrtcHandler>> webrtcHandlers;

			boost::unordered_flat_map<int, std::unique_ptr<WebrtcValueHandler>> webrtcValueHandlers;

			StringKeyedFlatMap<std::unique_ptr<HttpHandler>> httpHandlers;

			StringKeyedFlatMap<bool> httpLogicHandlers;

			HttpFilters httpFilters;

			std::shared_ptr<hope::storage::MysqlManagerPools> mysqlManagerPools;

			std::vector<hope::storage::RedisWrapper> redisWrappers;

			std::atomic<size_t> redisWrapperIndex{ 0 };

			std::atomic<size_t> localTaskQueueSize{ 0 };

			std::atomic<bool> asyncEvents{ false };

			TaskChannel& taskQueues;

			AsioConcurrentQueue<PostedTask> executeQueue;

			std::atomic<bool> asyncTaskExecutes{ false };

			std::atomic<bool> asyncExecutes{ false };

			WebrtcLogicConfig webrtcLogicConfig;

		};

	}

}
