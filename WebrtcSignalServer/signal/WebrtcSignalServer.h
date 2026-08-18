#pragma once
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <type_traits>
#include <boost/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <absl/functional/any_invocable.h>

#include "AwaitableTask.h"

#include "WebrtcSignalManager.h"
#include "../rpc/CoroRpc.h"
#include "../rpc/CoroRpcHandleInterface.h"
#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        struct WebrtcSignalConfig {

            size_t signalPort = 8088;

            size_t enableHttp = 0;

            size_t httpPort = 9099;

            size_t enablePublicPort = 1;

            size_t threadSize = std::thread::hardware_concurrency();

            size_t enableRpc = 0;

            hope::rpc::CoroRpcServerConfig coroRpcServerConfig;

            int overload = 256;

            int threshold = 256;

            int exitThreshold = 128;

            int asyncThreshold = 32;

            int maxTlsHandShakeTime = 10000;

            int maxTlsHttpHandShakeTime = 10000;

            int maxHttpKeepAliveTime = 300;

        };

        template <typename T>
        struct AwaitableReturnValue;

        template <typename T, typename Executor>
        struct AwaitableReturnValue<boost::asio::awaitable<T, Executor>> {
            using type = T;
        };

        template <typename AsyncHandle>
        using AwaitableReturnValueType = typename AwaitableReturnValue<std::decay_t<
            std::invoke_result_t<AsyncHandle, std::shared_ptr<WebrtcSignalManager>>>>::type;

        template <typename T>
        struct PostTaskCompletionSignature {
            using type = void(std::exception_ptr, T);
        };

        template <>
        struct PostTaskCompletionSignature<void> {
            using type = void(std::exception_ptr);
        };

        template <typename AsyncHandle, typename = void>
        struct IsAwaitableReturning : std::false_type {
        };

        template <typename AsyncHandle>
        struct IsAwaitableReturning<AsyncHandle, std::void_t<AwaitableReturnValueType<AsyncHandle>>>
            : std::true_type {
        };

        class WebrtcSignalServer : public std::enable_shared_from_this<WebrtcSignalServer> {

        public:

            WebrtcSignalServer(boost::asio::io_context& ioContext, WebrtcSignalConfig webrtcSignalConfig = {});

            ~WebrtcSignalServer();

            WebrtcSignalServer(const WebrtcSignalServer&) = delete;

            WebrtcSignalServer& operator=(const WebrtcSignalServer&) = delete;

            bool asyncEvent();

            void closeEvent();

            struct CompletionPostTask {
                template <typename... Args>
                void operator()(std::exception_ptr exception, Args&&... /*value*/) const {
                    if (exception) {
                        try { std::rethrow_exception(exception); }
                        catch (const std::exception& e) {
                            LOG_ERROR("WebrtcSignalServer postTask co_spawn Exception: %s", e.what());
                        }
                    }
                }
            };

            template <typename AsyncHandle, typename CompletionToken = CompletionPostTask,
                std::enable_if_t<IsAwaitableReturning<AsyncHandle>::value, int> = 0>
            auto postTask(size_t channelIndex, AsyncHandle&& asyncHandle,
                CompletionToken&& token = CompletionToken{})
                -> typename boost::asio::async_result<std::decay_t<CompletionToken>,
                    typename PostTaskCompletionSignature<AwaitableReturnValueType<AsyncHandle>>::type>::return_type
            {
                using ValueType = AwaitableReturnValueType<AsyncHandle>;

                return boost::asio::async_initiate<CompletionToken,
                    typename PostTaskCompletionSignature<ValueType>::type>(
                    [this, channelIndex, asyncHandle = std::move(asyncHandle)](auto completionHandler) mutable {

                        using CompletionHandlerType = std::decay_t<decltype(completionHandler)>;

                        std::shared_ptr<CompletionHandlerType> completionHandlerPtr = std::make_shared<CompletionHandlerType>(std::move(completionHandler));

                        if (channelIndex >= webrtcSignalManagers.size() || !webrtcSignalManagers[channelIndex]) {
                            LOG_ERROR("WebrtcSignalServer postTask invalid channelIndex: %zu, size: %zu", channelIndex, webrtcSignalManagers.size());
                            boost::asio::post(boost::asio::get_associated_executor(*completionHandlerPtr, ioContext),
                                [completionHandlerPtr]() mutable {
                                    if constexpr (std::is_void_v<ValueType>) {
                                        (*completionHandlerPtr)(std::make_exception_ptr(std::runtime_error("postTask: invalid channelIndex")));
                                    }
                                    else {
                                        (*completionHandlerPtr)(std::make_exception_ptr(std::runtime_error("postTask: invalid channelIndex")), ValueType{});
                                    }
                                });
                            return;
                        }

                        std::shared_ptr<WebrtcSignalManager> webrtcSignalManager = webrtcSignalManagers[channelIndex];

                        if constexpr (std::is_void_v<ValueType>) {
                            boost::asio::co_spawn(webrtcSignalManager->getLogicSystem()->getIoCompletionPorts(),
                                [webrtcSignalManager = webrtcSignalManager->shared_from_this(), asyncHandle = std::move(asyncHandle)]() mutable
                                -> boost::asio::awaitable<void> {
                                    co_await asyncHandle(std::move(webrtcSignalManager));
                                    co_return;
                                },
                                [completionHandlerPtr](std::exception_ptr exception) mutable {
                                    (*completionHandlerPtr)(std::move(exception));
                                });
                        }
                        else {
                            boost::asio::co_spawn(webrtcSignalManager->getLogicSystem()->getIoCompletionPorts(),
                                [webrtcSignalManager = webrtcSignalManager->shared_from_this(), asyncHandle = std::move(asyncHandle)]() mutable
                                -> boost::asio::awaitable<ValueType> {
                                    co_return co_await asyncHandle(std::move(webrtcSignalManager));
                                },
                                [completionHandlerPtr](std::exception_ptr exception, ValueType value = {}) mutable {
                                    (*completionHandlerPtr)(std::move(exception), std::move(value));
                                });
                        }
                    },
                    token);
            }

            bool postTask(size_t channelIndex, absl::AnyInvocable<void(std::shared_ptr<WebrtcSignalManager>)>&& asyncHandle);

            size_t getChannelNumbers();

            std::vector<std::shared_ptr<WebrtcSignalManager>> & getWebrtcSignalManagers();

            void registerRpcHandleImpl(std::unique_ptr<hope::rpc::CoroRpcHandleInterface> coroRpcHandleInterface);

        private:

            std::shared_ptr<WebrtcSignalManager> loadBalanceWebrtcManger();

            void initialize();

        private:

            std::vector<std::shared_ptr<WebrtcSignalManager>> webrtcSignalManagers;

            std::atomic<size_t> managerIndex{ 0 };

            std::atomic<bool> asyncEvents{ false };

            boost::asio::io_context& ioContext;

#ifndef __linux__

            boost::asio::ip::tcp::acceptor acceptor;

            boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

            WebrtcSignalConfig webrtcSignalConfig;

            TaskChannel taskQueues;

            std::vector<std::unique_ptr<hope::rpc::CoroRpcHandleInterface>> coroRpcHandleInterfaces;

        };
    }

}