#pragma once


#include <unordered_map>
#include <memory>
#include <utility>
#include <exception>
#include <stdexcept>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <ylt/struct_pack.hpp>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/any_invocable.h>
#include <absl/strings/str_format.h>

#include "../mysql/WebrtcMysqlManagerPools.h"

#include "../utils/Utils.h"

#include "AwaitableTask.h"

#include "HttpFilters.h"

#include "WebrtcSignalPacket.h"

#include "WebrtcSignalSocket.h"

namespace hope {

	namespace signal {

		class WebrtcSignalServer;

		class HttpSocket;

		class WebrtcLogicSystem : public std::enable_shared_from_this<WebrtcLogicSystem>
		{

		public:

			WebrtcLogicSystem(boost::asio::io_context& ioContext, int channelIndex, TaskChannel& taskQueues, int threshold, int exitThreshold, int asyncThreshold);

			~WebrtcLogicSystem();

			WebrtcLogicSystem(const WebrtcLogicSystem& logic) = delete;

			void operator=(const WebrtcLogicSystem& logic) = delete;

			struct CompletionPostTask {

				void operator()(std::exception_ptr exception) const {

					if (exception) {

						try {

							std::rethrow_exception(exception);

						}
						catch (const std::exception& e) {

							LOG_ERROR("WebrtcLogicSystem postTask co_spawn Exception: {}", e.what());

						}

					}

				}

			};

			struct CompletionCoPostTask {

				template <typename... Args>
				void operator()(std::exception_ptr exception, Args&&... /*value*/) const {

					if (exception) {

						try {

							std::rethrow_exception(exception);

						}
						catch (const std::exception& e) {

							LOG_ERROR("WebrtcLogicSystem coPostTask co_spawn Exception: {}", e.what());

						}

					}

				}

			};

			template <typename CompletionToken = CompletionPostTask>
			auto postTask(hope::signal::WebrtcSignalPacket webrtcSignalPacket, CompletionToken&& token = CompletionToken{})
				-> typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr)>::return_type
			{

				int type = webrtcSignalPacket.webrtcEnvelope.requestType;

				return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr)>(
					[this, type, webrtcSignalPacket = std::move(webrtcSignalPacket)](auto completionHandler) mutable {

						using CompletionHandlerType = std::decay_t<decltype(completionHandler)>;

						std::shared_ptr<CompletionHandlerType> completionHandlerPtr = std::make_shared<CompletionHandlerType>(std::move(completionHandler));

						absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<void>(hope::signal::WebrtcSignalPacket)>>::iterator iterator = this->webrtcHandlers.find(type);

						if (iterator != this->webrtcHandlers.end()) {

							absl::AnyInvocable<boost::asio::awaitable<void>(hope::signal::WebrtcSignalPacket)>& func = iterator->second;

							if (localTaskQueueSize.load() >= threshold.load() && webrtcLogicHandlers[type]) {

								std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket->shared_from_this();

								bool success = taskQueues.enqueue([type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket), completionHandlerPtr]()mutable -> boost::asio::awaitable<void> {

									try {

										co_await func(std::move(webrtcSignalPacket));

										(*completionHandlerPtr)(std::exception_ptr{});

									}
									catch (...) {

										(*completionHandlerPtr)(std::current_exception());

									}

									co_return;

									});

								if (!success) {

									WebrtcEnvelopeView env;

									env.requestType = type;

									env.state = 503;

									env.message = "webrtcSignalServer busy, please retry later";

									webrtcSignalSocket->asyncWrite(struct_pack::serialize<std::string>(env));

									boost::asio::post(boost::asio::get_associated_executor(*completionHandlerPtr, ioContext), [completionHandlerPtr]() mutable {

										(*completionHandlerPtr)(std::exception_ptr{});

										});

								}

							}
							else {

								localTaskQueueSize.fetch_add(1);

								boost::asio::co_spawn(ioContext, [type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<void> {

									co_await func(std::move(webrtcSignalPacket));

									},
									[this, completionHandlerPtr](std::exception_ptr exception) mutable {

										if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

											asyncTaskExecute();

										}

										(*completionHandlerPtr)(exception);

										});

							}

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

			template <typename CompletionToken = CompletionCoPostTask>
			auto coPostTask(hope::signal::WebrtcSignalPacket webrtcSignalPacket, CompletionToken&& token = CompletionToken{})
				-> typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr, boost::json::value)>::return_type
			{

				int type = webrtcSignalPacket.webrtcEnvelope.requestType;

				return boost::asio::async_initiate<CompletionToken, void(std::exception_ptr, boost::json::value)>(
					[this, type, webrtcSignalPacket = std::move(webrtcSignalPacket)](auto completionHandler) mutable {

						using CompletionHandlerType = std::decay_t<decltype(completionHandler)>;

						std::shared_ptr<CompletionHandlerType> completionHandlerPtr = std::make_shared<CompletionHandlerType>(std::move(completionHandler));

						absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<boost::json::value>(hope::signal::WebrtcSignalPacket)>>::iterator iterator = this->webrtcValueHandlers.find(type);

						if (iterator != this->webrtcValueHandlers.end()) {

							absl::AnyInvocable<boost::asio::awaitable<boost::json::value>(hope::signal::WebrtcSignalPacket)>& func = iterator->second;

							if (localTaskQueueSize.load() >= threshold.load() && webrtcValueLogicHandlers[type]) {

								std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalPacket.webrtcSignalSocket->shared_from_this();

								bool success = taskQueues.enqueue([type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket), completionHandlerPtr]()mutable -> boost::asio::awaitable<void> {

									try {

										boost::json::value value = co_await func(std::move(webrtcSignalPacket));

										(*completionHandlerPtr)(std::exception_ptr{}, std::move(value));

									}
									catch (...) {

										(*completionHandlerPtr)(std::current_exception(), boost::json::value{});

									}

									co_return;

									});

								if (!success) {

									WebrtcEnvelopeView env;

									env.requestType = type;

									env.state = 503;

									env.message = "webrtcSignalServer busy, please retry later";

									webrtcSignalSocket->asyncWrite(struct_pack::serialize<std::string>(env));

									boost::asio::post(boost::asio::get_associated_executor(*completionHandlerPtr, ioContext), [completionHandlerPtr]() mutable {

										(*completionHandlerPtr)(std::make_exception_ptr(std::runtime_error("webrtcSignalServer busy, please retry later")), boost::json::value{});

										});

								}

							}
							else {

								localTaskQueueSize.fetch_add(1);

								boost::asio::co_spawn(ioContext, [type, &func, webrtcSignalPacket = std::move(webrtcSignalPacket)]() mutable -> boost::asio::awaitable<boost::json::value> {

									co_return co_await func(std::move(webrtcSignalPacket));

									},
									[this, completionHandlerPtr](std::exception_ptr exception, boost::json::value value = {}) mutable {

										if (localTaskQueueSize.fetch_sub(1) == asyncThreshold.load() + 1) {

											asyncTaskExecute();

										}

										(*completionHandlerPtr)(std::move(exception), std::move(value));

										});

							}

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

			void asyncTaskExecute();

		private:

			void initHandlers();

			void initFilters();

			void initHttpHandlers();

		public:

			boost::asio::io_context& ioContext;

			int channelIndex;

			absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<void>(hope::signal::WebrtcSignalPacket)>> webrtcHandlers;

			absl::flat_hash_map<int, absl::AnyInvocable<boost::asio::awaitable<boost::json::value>(hope::signal::WebrtcSignalPacket)>> webrtcValueHandlers;

			absl::flat_hash_map<std::string, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<HttpSocket>, boost::beast::http::request<boost::beast::http::string_body>)>> httpHandlers;

			absl::flat_hash_map<int, bool> webrtcLogicHandlers;

			absl::flat_hash_map<int, bool> webrtcValueLogicHandlers;

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
