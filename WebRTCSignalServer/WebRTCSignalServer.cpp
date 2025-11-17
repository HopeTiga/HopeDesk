#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "AsioProactors.h"
#include "WebRTCSignalManager.h"
#include "Logger.h"
#include "Utils.h"

namespace hope {
    
    namespace core {
        WebRTCSignalServer::WebRTCSignalServer(boost::asio::io_context& ioContext, size_t port, size_t size)
            : ioContext(ioContext)
            , port(port)
            , acceptor(ioContext, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::any(), port))
            , size(size)
            , webrtcSignalManagers(size)
        {
            initialize();
        }

        void WebRTCSignalServer::run() {

            // 启动服务器逻辑（监听端口，接受连接等）
            LOG_INFO("WebRTCSginalServer EventLoop ，Listen Accept Port: %zu", port);
            // 这里可以添加更多的启动逻辑
            boost::asio::co_spawn(ioContext, [this]()->boost::asio::awaitable<void> {

                while (!isShuttingDown.load()) {
                    try {

                        std::shared_ptr<WebRTCSignalManager> manager = getWebRTCSignalManager();

                        std::shared_ptr<WebRTCSignalSocket> webrtcSignalSocket = manager->generateWebRTCSignalSocket();

                        co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                        boost::asio::ip::tcp::endpoint remoteEndpoint = webrtcSignalSocket->getSocket().remote_endpoint();

                        std::string clientIP = remoteEndpoint.address().to_string();

                        unsigned short clientPort = remoteEndpoint.port();

                        Logger::getInstance()->info("new Connection from Address: " + std::string(clientIP.c_str()) + ":" + std::to_string(clientPort));

                        boost::asio::co_spawn(manager->getIoCompletePorts(), [this, sharedWebrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                            co_await sharedWebrtcSignalSocket->handShake();

                            sharedWebrtcSignalSocket->start();

                            }, boost::asio::detached);

                    }
                    catch (const std::exception& e) {
                        if (!isShuttingDown.load()) {
                            LOG_ERROR("Accept Connection Error: %s", e.what());
                        }
                    }
                }
                }, [this](std::exception_ptr p) {});

        }


        WebRTCSignalServer::~WebRTCSignalServer() {

            if (isShuttingDown.exchange(true)) return;

            shutdown();

        }

        void WebRTCSignalServer::shutdown() {

            LOG_INFO("WebRTCSignalServer Start Shutdown...");
            // 设置关闭标志，防止新连接
            isShuttingDown.store(true);

            webrtcSignalManagers.clear();

            LOG_INFO("WebRTCSignalServer Already Shutdown");

        }

        void WebRTCSignalServer::postAsyncTask(int channelIndex,
            std::function<boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager> manager)> asyncHandle)
        {
            // 1. 添加边界检查
            if (channelIndex < 0 || channelIndex >= webrtcSignalManagers.size()) {
                LOG_ERROR("Invalid channelIndex: %d, size: %zu", channelIndex, webrtcSignalManagers.size());
                return;
            }

            // 2. 使用 lambda 捕获 shared_ptr，确保生命周期安全
            auto manager = webrtcSignalManagers[channelIndex];
            if (!manager) {
                LOG_ERROR("WebRTCSignalManager at index %d is null", channelIndex);
                return;
            }

            // 3. 使用 lambda 而不是 std::bind
            boost::asio::co_spawn(manager->getWebRTCLogicSystem()->getIoCompletePorts(),
                [manager, asyncHandle = std::move(asyncHandle)]() -> boost::asio::awaitable<void> {
                    co_await asyncHandle(manager);  // 在协程内部执行
                }, [this](std::exception_ptr ptr) {
                    // 正确的异常处理方式
                    if (ptr) { // 重要：检查是否确实有异常发生
                        try {
                            std::rethrow_exception(ptr); // 重新抛出异常
                        }
                        catch (const std::exception& e) {
                            // 现在可以正常捕获并处理了
                            LOG_ERROR("webrtcSignalServer boost::asio::co_spawn Exception: %s", e.what());
                        }
                    }
                });
        }


        std::shared_ptr<WebRTCSignalManager> WebRTCSignalServer::getWebRTCSignalManager()
        {
            size_t index = managerIndex.fetch_add(1) % size;

            return webrtcSignalManagers[index];

        }

        void WebRTCSignalServer::initialize()
        {

            for (int i = 0; i < size; i++) {

                std::pair<int, boost::asio::io_context&> channelPairs = AsioProactors::getInstance()->getIoCompletePorts();

                webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(channelPairs.second, channelPairs.first, this);

            }

        }
    }

}