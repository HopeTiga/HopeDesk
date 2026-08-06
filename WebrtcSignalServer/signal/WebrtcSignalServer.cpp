#include "WebrtcSignalServer.h"

#include <chrono>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "../iocp/AsioProactors.h"
#include "WebrtcSignalSocket.h"
#include "HttpSocket.h"

namespace hope {

    namespace signal {

        WebrtcSignalServer::WebrtcSignalServer(boost::asio::io_context& ioContext, WebrtcSignalConfig webrtcSignalConfig)
            : ioContext(ioContext)
            , webrtcSignalConfig(webrtcSignalConfig)
#ifndef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif
            , webrtcSignalManagers(webrtcSignalConfig.threadSize)
            , taskQueues(ioContext, webrtcSignalConfig.overload * (webrtcSignalConfig.threadSize + 1))
            , coroRpcHandlerImpl(*this)
        {

#ifndef __linux__

            boost::asio::ip::address address = webrtcSignalConfig.enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            try {

                acceptor.open(boost::asio::ip::tcp::v4());
                acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                acceptor.bind(boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.signalPort));
                acceptor.listen(boost::asio::socket_base::max_listen_connections);

                httpAcceptor.open(boost::asio::ip::tcp::v4());
                httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
                httpAcceptor.bind(boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.httpPort));
                httpAcceptor.listen(boost::asio::socket_base::max_listen_connections);

            }
            catch (const std::exception& e) {
                LOG_ERROR("Acceptor setup failed: %s", e.what());
                throw;
            }

#endif

            initialize();

        }

        bool WebrtcSignalServer::asyncEvent() {

            if (asyncEvents.exchange(true)) return true;

            LOG_INFO("WebrtcSginalServer Protocol: WebSocket , Listen Accept Port: %zu", webrtcSignalConfig.signalPort);

            if (webrtcSignalConfig.enableHttp == 1) {

                LOG_INFO("WebrtcSginalServer Protocol: Https , Listen Accept Port: %zu", webrtcSignalConfig.httpPort);

            }

            if (webrtcSignalConfig.enableRpc == 1) {

                hope::rpc::CoroRpc* coroRpc = hope::rpc::CoroRpc::getInstance();

                if (!coroRpc->initCoroRpc(webrtcSignalConfig.coroRpcServerConfig)) {
                
                    LOG_ERROR("CoroRpc::initCoroRpc Failed");

                    asyncEvents.store(false);

                    return false;

                }

                coroRpc->createClientPools();

                std::vector<std::string> hosts;

                coroRpc->createLoadBalancer(hosts);

                coroRpcHandlerImpl.coroRpc = coroRpc;

                coroRpcHandlerImpl.registerRpcHandler();

                coroRpc->asyncEvent();

                LOG_INFO("WebrtcSginalServer Protocol: CoroRpc , Listen Accept Port: %zu", webrtcSignalConfig.coroRpcServerConfig.port);

            }

#ifndef __linux__

            boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::shared_ptr<WebrtcSignalManager> webrtcSignalManager = loadBalanceWebrtcManger();

                    std::shared_ptr<hope::signal::WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalManager->generateWebrtcSignalSocket();

                    bool shouldBackoff = false;

                    try {

                        co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    }
                    catch (const boost::system::system_error& e) {

                        if (e.code() == boost::asio::error::operation_aborted || !asyncEvents.load() || !acceptor.is_open()) {

                            LOG_INFO("WebrtcSignalServer accept loop exits: %s", e.code().message().c_str());

                            break;

                        }

                        LOG_WARN("WebrtcSignalServer accept failed, backoff and retry: %s", e.code().message().c_str());

                        shouldBackoff = true;

                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("WebrtcSignalServer accept loop fatal exception: %s", e.what());

                        break;

                    }

                    if (shouldBackoff) {

                        boost::asio::steady_timer backoffTimer(ioContext);

                        backoffTimer.expires_after(std::chrono::milliseconds(100));

                        co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                        continue;

                    }

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = webrtcSignalManager->shared_from_this()](std::string accountId, std::string sessionId) {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

                        sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

#else
                        boost::asio::io_context& ioContext = sharedManager->getIoCompletionPorts();

                        boost::asio::post(ioContext, [sharedManager = std::move(sharedManager), accountId = std::move(accountId), sessionId = std::move(sessionId)] {

                            sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

                            });
#endif

                        });

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [webrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        if (co_await webrtcSignalSocket->handShake()) {

                            webrtcSignalSocket->asyncEvent();

                        }

                        }, boost::asio::detached);

                }

                }, [this](std::exception_ptr ptr) {

                    if (ptr) {

                        try { std::rethrow_exception(ptr); }

                        catch (const std::exception& e) {

                            LOG_ERROR("WebrtcSignalServer accept loop unhandled exception: %s", e.what());

                        }

                    }

                });

            if (webrtcSignalConfig.enableHttp == 1) {

                boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                    while (asyncEvents.load()) {

                        std::shared_ptr<WebrtcSignalManager> manager = loadBalanceWebrtcManger();

                        std::shared_ptr<HttpSocket> httpSocket = manager->generateHttpSocket();

                        bool shouldBackoff = false;

                        try {

                            co_await httpAcceptor.async_accept(httpSocket->getSocket(), boost::asio::use_awaitable);

                        }
                        catch (const boost::system::system_error& e) {

                            if (e.code() == boost::asio::error::operation_aborted || !asyncEvents.load() || !httpAcceptor.is_open()) {

                                LOG_INFO("WebrtcSignalServer http accept loop exits: %s", e.code().message().c_str());

                                break;

                            }

                            LOG_WARN("WebrtcSignalServer http accept failed, backoff and retry: %s", e.code().message().c_str());

                            shouldBackoff = true;

                        }
                        catch (const std::exception& e) {

                            LOG_ERROR("WebrtcSignalServer http accept loop fatal exception: %s", e.what());

                            break;

                        }

                        if (shouldBackoff) {

                            boost::asio::steady_timer backoffTimer(ioContext);

                            backoffTimer.expires_after(std::chrono::milliseconds(100));

                            co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                            continue;

                        }

                        boost::asio::co_spawn(httpSocket->getIoContext(), [this, httpSocket = httpSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                            co_await httpSocket->asyncEvent();

                            co_return;

                            }, boost::asio::detached);

                    }

                    co_return;

                    }, [this](std::exception_ptr ptr) {

                        if (ptr) {

                            try { std::rethrow_exception(ptr); }

                            catch (const std::exception& e) {

                                LOG_ERROR("WebrtcSignalServer http accept loop unhandled exception: %s", e.what());

                            }

                        }

                    });

            }

#elif defined(__linux__)

            boost::asio::ip::address address = webrtcSignalConfig.enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->asyncAccept(asyncEvents, boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.signalPort), boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.httpPort), static_cast<int>(webrtcSignalConfig.enableHttp));

            }

#endif

            boost::asio::co_spawn(ioContext, [this]()mutable->boost::asio::awaitable<void> {

                while (asyncEvents.load()) {

                    std::optional<AwaitableTask> optional = co_await taskQueues.dequeue();

                    if (!optional.has_value()) break;

                    AwaitableTask func = std::move(optional.value());

                    if (func) {

                        co_await func();

                    }

                    if (!asyncEvents.load()) break;

                }

                LOG_INFO("WebrtcSignalServer asyncTaskExecute closeAsyncEvent");

                co_return;

                }, boost::asio::detached);

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->getLogicSystem()->asyncTaskExecute();

            }

            return true;;

        }


        WebrtcSignalServer::~WebrtcSignalServer() {

            closeEvent();

        }

        void WebrtcSignalServer::closeEvent() {

            if (!asyncEvents.exchange(false)) return;

            LOG_INFO("WebrtcSignalServer CloseEvent...");

            hope::rpc::CoroRpc::getInstance()->closeEvent();

            taskQueues.close();
      
            webrtcSignalManagers.clear();

            LOG_INFO("WebrtcSignalServer Already CloseEvent");

        }

        bool WebrtcSignalServer::postTask(size_t channelIndex, absl::AnyInvocable<void(std::shared_ptr<WebrtcSignalManager>)>&& asyncHandle)
        {
            if (channelIndex >= webrtcSignalManagers.size()) {
                LOG_ERROR("Invalid channelIndex: %zu, size: %zu", channelIndex, webrtcSignalManagers.size());
                return false;
            }

            auto manager = webrtcSignalManagers[channelIndex];
            if (!manager) {
                LOG_ERROR("WebRTCSignalManager at index %zu is null", channelIndex);
                return false;
            }

            boost::asio::post(manager->getLogicSystem()->getIoCompletionPorts(),
                [manager = manager->shared_from_this(), asyncHandle = std::move(asyncHandle)]()mutable -> void {
                    asyncHandle(std::move(manager));
                });

                return true;
        }

        std::shared_ptr<WebrtcSignalManager> WebrtcSignalServer::loadBalanceWebrtcManger()
        {
            size_t index = managerIndex.fetch_add(1) % webrtcSignalConfig.threadSize;

            return webrtcSignalManagers[index];

        }

        size_t WebrtcSignalServer::getChannelNumbers()
        {
            return webrtcSignalManagers.size();
        }

        void WebrtcSignalServer::initialize()
        {

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                boost::asio::io_context& ioContext = hope::iocp::AsioProactors::getInstance()->getIoCompletePort(i);

                WebrtcSignalChannelConfig channelConfig{
                    webrtcSignalConfig.threadSize,
                    webrtcSignalConfig.threshold,
                    webrtcSignalConfig.exitThreshold,
                    webrtcSignalConfig.asyncThreshold,
                    webrtcSignalConfig.maxTlsHandShakeTime,
                    webrtcSignalConfig.maxTlsHttpHandShakeTime,
                    webrtcSignalConfig.maxHttpKeepAliveTime
                };

                webrtcSignalManagers[i] = std::make_shared<WebrtcSignalManager>(i, ioContext, this, taskQueues, channelConfig);

            }

        }

        std::vector<std::shared_ptr<WebrtcSignalManager>>& WebrtcSignalServer::getWebrtcSignalManagers() {
        
            return webrtcSignalManagers;

        }

    }

}