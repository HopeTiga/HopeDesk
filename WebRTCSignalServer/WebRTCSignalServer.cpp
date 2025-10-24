#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "AsioProactors.h"
#include "WebRTCSignalManager.h"
#include "Logger.h"

namespace Hope {
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
        LOG_INFO("WebRTC信令服务器正在运行，监听端口: %zu", port);
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

                    boost::asio::co_spawn(manager->getIoComplatePorts(), [this, sharedWebrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        co_await sharedWebrtcSignalSocket->handShake();

                        sharedWebrtcSignalSocket->start();

                        }, boost::asio::detached);

                }
                catch (const std::exception& e) {
                    if (!isShuttingDown.load()) {
                        LOG_ERROR("接受连接时出错: %s", e.what());
                    }
                }
            }
            }, [this](std::exception_ptr p) {});

    }


    WebRTCSignalServer::~WebRTCSignalServer() {
        LOG_INFO("WebRTC信令服务器开始关闭...");
        shutdown();
        LOG_INFO("WebRTC信令服务器已完全关闭");
    }

    void WebRTCSignalServer::shutdown() {
        LOG_INFO("设置关闭标志，停止接受新连接");
        // 设置关闭标志，防止新连接
        isShuttingDown.store(true);

		webrtcSignalManagers.clear();

    }

    void WebRTCSignalServer::postAsyncTask(int channelIndex,
        std::function<void(std::shared_ptr<WebRTCSignalManager> manager)> asyncHandle)
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
        boost::asio::post(manager->getIoComplatePorts(),
            [manager, asyncHandle = std::move(asyncHandle)]() {
                asyncHandle(manager);
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

            std::pair<int, boost::asio::io_context&> channelPairs =  AsioProactors::getInstance()->getIoComplatePorts();
            
			webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(channelPairs.second, channelPairs.first,this);

        }

    }

}