#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <functional>
#include <unordered_set>
#include <atomic>
#include <boost/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>


namespace hope {

    namespace core {

        class WebRTCSignalManager;

        class WebRTCSignalServer : public std::enable_shared_from_this<WebRTCSignalServer> {

        public:

            WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port = 8088, int enableHttp = 0, size_t httpPort = 8080, size_t size = std::thread::hardware_concurrency());

            ~WebRTCSignalServer();

            WebRTCSignalServer(const WebRTCSignalServer&) = delete;

            WebRTCSignalServer& operator=(const WebRTCSignalServer&) = delete;

            void asyncEvent();

            void closeEvent();

            bool postTaskAsync(size_t channelIndex, std::function <boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager>) > asyncHandle);

            size_t getChannelNumbers();

        private:

            std::shared_ptr<WebRTCSignalManager> loadBalanceWebrtcManger();

            void initialize();

        private:

            std::vector<std::shared_ptr<WebRTCSignalManager>> webrtcSignalManagers;

            std::atomic<size_t> managerIndex{ 0 };

            std::atomic<bool> runAccepct{ false };

            std::atomic<bool> closeEvents{ false };

            boost::asio::io_context& ioContext;

#ifndef __linux__

            boost::asio::ip::tcp::acceptor acceptor;

            boost::asio::ip::tcp::acceptor httpAcceptor;

#endif

            size_t port;

            size_t httpPort;

            int enableHttp;

            size_t size;
        };
    }

}