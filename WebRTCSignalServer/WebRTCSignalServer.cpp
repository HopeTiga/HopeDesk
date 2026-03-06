#include "WebRTCSignalServer.h"
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "AsioProactors.h"
#include "WebRTCSignalManager.h"
#include "WebRTCSignalSocket.h"
#include "WebRTCMysqlManagerPools.h"
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

            if (runAccepct.load()) return;

            runAccepct.store(true);

            boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                while (runAccepct.load()) {

                    std::shared_ptr<WebRTCSignalManager> manager = loadBalanceMsquicManger();

                    std::shared_ptr<hope::core::WebRTCSignalSocket> webrtcSignalSocket = std::make_shared<hope::core::WebRTCSignalSocket>(hope::iocp::AsioProactors::getInstance()->getIoCompletePorts().second, manager.get());

                    co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = manager->shared_from_this()](std::string accountId, std::string sessionId) {

                        sharedManager->removeConnection(accountId, sessionId);

                        });

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [this, selfWebRTCSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        co_await selfWebRTCSignalSocket->handShake();

                        selfWebRTCSignalSocket->runEventLoop();

                        }, boost::asio::detached);


                }

                }, boost::asio::detached);

            return;

        }


        WebRTCSignalServer::~WebRTCSignalServer() {

            shutdown();

        }

        void WebRTCSignalServer::shutdown() {

            if (shuttingDown.exchange(true)) return;

			runAccepct.store(false);

            LOG_INFO("WebRTCSignalServer Start Shutdown...");
            // 设置关闭标志，防止新连接
            webrtcSignalManagers.clear();

            LOG_INFO("WebRTCSignalServer Already Shutdown");

        }

        void WebRTCSignalServer::postTaskAsync(size_t channelIndex, std::function <boost::asio::awaitable<void>(std::shared_ptr<WebRTCSignalManager>) > asyncHandle)
        {
            if (channelIndex >= webrtcSignalManagers.size()) {
                LOG_ERROR("Invalid channelIndex: %zu, size: %zu", channelIndex, webrtcSignalManagers.size());
                return;
            }

            auto manager = webrtcSignalManagers[channelIndex];
            if (!manager) {
                LOG_ERROR("WebRTCSignalManager at index %zu is null", channelIndex);
                return;
            }

            boost::asio::co_spawn(manager->getLogicSystem()->getIoCompletePorts(),
                [manager = manager->shared_from_this(), asyncHandle = std::move(asyncHandle)]() -> boost::asio::awaitable<void> {
                    co_await asyncHandle(manager);
                }, [this](std::exception_ptr ptr) {
                    if (ptr) {
                        try {
                            std::rethrow_exception(ptr);
                        }
                        catch (const std::exception& e) {
                            LOG_ERROR("WebRTCSignalServer boost::asio::co_spawn Exception: %s", e.what());
                        }
                    }
                    });
        }


        std::shared_ptr<WebRTCSignalManager> WebRTCSignalServer::loadBalanceMsquicManger()
        {
            size_t index = managerIndex.fetch_add(1) % size;

            return webrtcSignalManagers[index];

        }

        void WebRTCSignalServer::initialize()
        {

            for (int i = 0; i < size; i++) {

                std::pair<int, boost::asio::io_context&> channelPairs = hope::iocp::AsioProactors::getInstance()->getIoCompletePorts();

                webrtcSignalManagers[i] = std::make_shared<WebRTCSignalManager>(channelPairs.first,channelPairs.second, this);

            }

        }
    }

}