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

#include "../rpc/CoroRpc.h"
#include "../rpc/CoroRpcHandlerImpl.h"

namespace hope {

    namespace signal {

        class WebrtcSignalManager;

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

            int socketWaitTime = 10000;

        };

        class WebrtcSignalServer : public std::enable_shared_from_this<WebrtcSignalServer> {

        public:

            WebrtcSignalServer(boost::asio::io_context& ioContext, WebrtcSignalConfig webrtcSignalConfig = {});

            ~WebrtcSignalServer();

            WebrtcSignalServer(const WebrtcSignalServer&) = delete;

            WebrtcSignalServer& operator=(const WebrtcSignalServer&) = delete;

            void asyncEvent();

            void closeEvent();

            bool postTaskAsync(size_t channelIndex, absl::AnyInvocable<boost::asio::awaitable<void>(std::shared_ptr<WebrtcSignalManager>)>&& asyncHandle);

            size_t getChannelNumbers();

            std::vector<std::shared_ptr<WebrtcSignalManager>> & getWebrtcSignalManagers();

        private:

            std::shared_ptr<WebrtcSignalManager> loadBalanceWebrtcManger();

            void initialize();

        public:

            std::shared_ptr<hope::rpc::CoroRpc> coroRpc;

        private:

            std::vector<std::shared_ptr<WebrtcSignalManager>> webrtcSignalManagers;

            std::atomic<size_t> managerIndex{ 0 };

            std::atomic<bool> asyncEvents{ false };

            std::atomic<bool> closeEvents{ false };

            boost::asio::io_context& ioContext;

#ifndef __linux__

            boost::asio::ip::tcp::acceptor acceptor;

            boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

            WebrtcSignalConfig webrtcSignalConfig;

            TaskChannel taskQueues;

            hope::rpc::CoroRpcHandlerImpl coroRpcHandlerImpl;

        };
    }

}