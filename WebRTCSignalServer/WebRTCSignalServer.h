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

        class WebRTCSignalServer {
        public:
            WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port = 8088, size_t size = std::thread::hardware_concurrency() * 2);

            ~WebRTCSignalServer();  // 🔧 新增析构函数声明

            // 禁止拷贝和赋值
            WebRTCSignalServer(const WebRTCSignalServer&) = delete;

            WebRTCSignalServer& operator=(const WebRTCSignalServer&) = delete;

            void run();

            void stop();

            // 新增：优雅关闭方法
            void shutdown();

            void postAsyncTask(int channelIndex, std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager>)> asyncHandle);

        private:

            std::shared_ptr<WebRTCSignalManager> getWebRTCSignalManager();

            void initialize();

        private:

            std::vector<std::shared_ptr<WebRTCSignalManager>> webrtcSignalManagers;

            std::atomic<size_t> managerIndex{ 0 };

            std::atomic<bool> isShuttingDown{ false };  // 🔧 新增：关闭标志

            boost::asio::io_context& ioContext;

            boost::asio::ip::tcp::acceptor acceptor;

            size_t port;

            size_t size;
        };
	}
   
}