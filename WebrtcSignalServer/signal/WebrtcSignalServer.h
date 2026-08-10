#pragma once
#include <memory>
#include <string>
#include <vector>
#include <atomic>
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

        class WebrtcSignalServer : public std::enable_shared_from_this<WebrtcSignalServer> {

        public:

            WebrtcSignalServer(boost::asio::io_context& ioContext, WebrtcSignalConfig webrtcSignalConfig = {});

            ~WebrtcSignalServer();

            WebrtcSignalServer(const WebrtcSignalServer&) = delete;

            WebrtcSignalServer& operator=(const WebrtcSignalServer&) = delete;

            bool asyncEvent();

            void closeEvent();

            struct PostTaskDetachedLogger {
                void operator()(std::exception_ptr exception) const {
                    if (exception) {
                        try { std::rethrow_exception(exception); }
                        catch (const std::exception& e) {
                            LOG_ERROR("WebrtcSignalServer postTaskAsync co_spawn Exception: %s", e.what());
                        }
                    }
                }
            };

            template <typename CompletionToken = PostTaskDetachedLogger>
            auto postTaskAsync(size_t channelIndex,
                absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<WebrtcSignalManager>)>&& asyncHandle,
                CompletionToken&& token = CompletionToken{})
                -> typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr)>::return_type
            {
                boost::asio::async_completion<CompletionToken, void(std::exception_ptr)> completion(token);

                if (channelIndex >= webrtcSignalManagers.size() || !webrtcSignalManagers[channelIndex]) {
                    LOG_ERROR("WebrtcSignalServer postTaskAsync invalid channelIndex: %zu, size: %zu", channelIndex, webrtcSignalManagers.size());
                    auto ex = boost::asio::get_associated_executor(completion.completion_handler, ioContext);
                    boost::asio::post(ex,
                        [h = std::move(completion.completion_handler)]() mutable {
                            h(std::make_exception_ptr(std::runtime_error("postTaskAsync: invalid channelIndex")));
                        });
                    return completion.result.get();
                }

                std::shared_ptr<WebrtcSignalManager> webrtcSignalManager = webrtcSignalManagers[channelIndex];
                boost::asio::co_spawn(webrtcSignalManager->getLogicSystem()->getIoCompletionPorts(),
                    [webrtcSignalManager = webrtcSignalManager->shared_from_this(), asyncHandle = std::move(asyncHandle)]() mutable
                    -> boost::asio::awaitable<void> { co_await asyncHandle(std::move(webrtcSignalManager)); },
                    std::move(completion.completion_handler));
                return completion.result.get();
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