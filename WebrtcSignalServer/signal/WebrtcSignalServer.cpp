
#include "WebrtcSignalServer.h"

#include <chrono>
#include <latch>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/asio/co_spawn.hpp>
#include <iostream> 

#include "../iocp/AsioProactors.h"
#include "WebrtcSignalSocket.h"
#include "HttpSocket.h"
#include "HttpFilters.h"

#include "../rpc/CoroRpc.h"

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
                LOG_ERROR("Acceptor Setup Failed: {}", e.what());
                throw;
            }

#endif

            initialize();

        }

        void WebrtcSignalServer::registerRpcHandleImpl(std::unique_ptr<hope::rpc::CoroRpcHandleInterface> coroRpcHandleInterface) {

            coroRpcHandleInterfaces.push_back(std::move(coroRpcHandleInterface));

        }

        bool WebrtcSignalServer::asyncBoot() {

            if (asyncBoots.exchange(true)) return true;

            LOG_INFO("Protocol: WebSocket , Listen Accept Port: {}", webrtcSignalConfig.signalPort);

            if (webrtcSignalConfig.enableHttp == 1) {

                LOG_INFO("Protocol: Https , Listen Accept Port: {}", webrtcSignalConfig.httpPort);

            }

            if (webrtcSignalConfig.enableRpc == 1) {

                hope::rpc::CoroRpc* coroRpc = hope::rpc::CoroRpc::getInstance();

                if (!coroRpc->initCoroRpc(webrtcSignalConfig.coroRpcServerConfig)) {
                
                    LOG_ERROR("CoroRpc::InitCoroRpc Failed");

                    asyncBoots.store(false);

                    return false;

                }

                coroRpc->createClientPools();

                std::vector<std::string> hosts;

                coroRpc->createLoadBalancer(hosts);

                for (std::unique_ptr<hope::rpc::CoroRpcHandleInterface>& coroRpcHandleInterface : coroRpcHandleInterfaces) {

                    coroRpcHandleInterface->registerRpcHandle();

                }

                coroRpc->asyncBoot();

                LOG_INFO("Protocol: CoroRpc , Listen Accept Port: {}", webrtcSignalConfig.coroRpcServerConfig.port);

            }

#ifndef __linux__

            boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                while (asyncBoots.load()) {

                    std::shared_ptr<WebrtcSignalManager> webrtcSignalManager = loadBalanceWebrtcManger();

                    std::shared_ptr<hope::signal::WebrtcSignalSocket> webrtcSignalSocket = webrtcSignalManager->generateWebrtcSignalSocket();

                    bool shouldBackoff = false;

                    try {

                        co_await acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    }
                    catch (const boost::system::system_error& e) {

                        if (e.code() == boost::asio::error::operation_aborted || !asyncBoots.load() || !acceptor.is_open()) {

                            LOG_INFO("Accept Loop Exits: {}", e.code().message().c_str());

                            break;

                        }

                        LOG_WARN("Accept Failed, Backoff And Retry: {}", e.code().message().c_str());

                        shouldBackoff = true;

                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("Accept Loop Fatal Exception: {}", e.what());

                        break;

                    }

                    if (shouldBackoff) {

                        boost::asio::steady_timer backoffTimer(ioContext);

                        backoffTimer.expires_after(std::chrono::milliseconds(100));

                        co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                        continue;

                    }

#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC

                    webrtcSignalSocket->setOnDisConnectHandle([webrtcSignalManager = std::move(webrtcSignalManager)](std::string accountId, std::string sessionId) {

                        boost::asio::io_context& ioContext = webrtcSignalManager->getIoCompletionPorts();

                        boost::asio::post(ioContext, [webrtcSignalManager = std::move(webrtcSignalManager), accountId = std::move(accountId), sessionId = std::move(sessionId)] {

                            webrtcSignalManager->removeConnection(std::move(accountId), std::move(sessionId));

                            });

                        });
#else

                    webrtcSignalSocket->setOnDisConnectHandle([&webrtcSignalManager](std::string accountId, std::string sessionId) {

                        webrtcSignalManager->removeConnection(std::move(accountId), std::move(sessionId));

                        });
#endif

                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [webrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        if (co_await webrtcSignalSocket->handShake()) {

                            webrtcSignalSocket->asyncBoot();

                        }

                        }, boost::asio::detached);

                }

                }, [this](std::exception_ptr ptr) {

                    if (ptr) {

                        try { std::rethrow_exception(ptr); }

                        catch (const std::exception& e) {

                            LOG_ERROR("Accept Loop Unhandled Exception: {}", e.what());

                        }

                    }

                });

            if (webrtcSignalConfig.enableHttp == 1) {

                boost::asio::co_spawn(ioContext, [this]() ->boost::asio::awaitable<void> {

                    while (asyncBoots.load()) {

                        std::shared_ptr<WebrtcSignalManager> webrtcSignalManager = loadBalanceWebrtcManger();

                        std::shared_ptr<HttpSocket> httpSocket = webrtcSignalManager->generateHttpSocket();

                        bool shouldBackoff = false;

                        try {

                            co_await httpAcceptor.async_accept(httpSocket->getSocket(), boost::asio::use_awaitable);

                        }
                        catch (const boost::system::system_error& e) {

                            if (e.code() == boost::asio::error::operation_aborted || !asyncBoots.load() || !httpAcceptor.is_open()) {

                                LOG_INFO("Http Accept Loop Exits: {}", e.code().message().c_str());

                                break;

                            }

                            LOG_WARN("Http Accept Failed, Backoff And Retry: {}", e.code().message().c_str());

                            shouldBackoff = true;

                        }
                        catch (const std::exception& e) {

                            LOG_ERROR("Http Accept Loop Fatal Exception: {}", e.what());

                            break;

                        }

                        if (shouldBackoff) {

                            boost::asio::steady_timer backoffTimer(ioContext);

                            backoffTimer.expires_after(std::chrono::milliseconds(100));

                            co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                            continue;

                        }

                        boost::asio::co_spawn(httpSocket->getIoContext(), [this, httpSocket = httpSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                            co_await httpSocket->asyncBoot();

                            co_return;

                            }, boost::asio::detached);

                    }

                    co_return;

                    }, [this](std::exception_ptr ptr) {

                        if (ptr) {

                            try { std::rethrow_exception(ptr); }

                            catch (const std::exception& e) {

                                LOG_ERROR("Http Accept Loop Unhandled Exception: {}", e.what());

                            }

                        }

                    });

            }

#elif defined(__linux__)

            boost::asio::ip::address address = webrtcSignalConfig.enablePublicPort ? boost::asio::ip::address_v4::any() : boost::asio::ip::address_v4::loopback();

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->asyncAccept(asyncBoots, boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.signalPort), boost::asio::ip::tcp::endpoint(address, webrtcSignalConfig.httpPort), static_cast<int>(webrtcSignalConfig.enableHttp));

            }

#endif

            boost::asio::co_spawn(ioContext, [this]()mutable->boost::asio::awaitable<void> {

                while (asyncBoots.load()) {

                    std::optional<AwaitableTask> optional = co_await taskQueues.dequeue();

                    if (!optional.has_value()) break;

                    AwaitableTask func = std::move(optional.value());

                    if (func) {

                        co_await func();

                    }

                    if (!asyncBoots.load()) break;

                }

                LOG_INFO("AsyncTaskExecute Close AsyncBoot");

                co_return;

                }, boost::asio::detached);

            for (int i = 0; i < webrtcSignalConfig.threadSize; i++) {

                webrtcSignalManagers[i]->getLogicSystem()->asyncTaskExecute();

            }

            return true;;

        }


        WebrtcSignalServer::~WebrtcSignalServer() {

            closeBoot();

        }

        void WebrtcSignalServer::closeBoot() {

            if (!asyncBoots.exchange(false)) return;

            LOG_INFO("Start CloseBoot");

            hope::rpc::CoroRpc::getInstance()->closeBoot();

            taskQueues.close();

            std::latch closeLatch(webrtcSignalManagers.size());

            for (std::shared_ptr<WebrtcSignalManager>& webrtcSignalManager : webrtcSignalManagers) {

                if (!webrtcSignalManager) {
                    closeLatch.count_down();
                    continue;
                }

#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC
                boost::asio::io_context & ioContext = webrtcSignalManager->getLogicSystem()->getIoCompletionPorts();
#else
                boost::asio::io_context & ioContext = webrtcSignalManager->getIoCompletionPorts();
#endif

                boost::asio::post(ioContext,
                    [webrtcSignalManager, &closeLatch]() {

                        for (StringKeyedNodeMap<std::shared_ptr<WebrtcSignalSocket>>::iterator iterator =
                                 webrtcSignalManager->webrtcSocketMap.begin();
                             iterator != webrtcSignalManager->webrtcSocketMap.end(); ++iterator) {

                            iterator->second->closeBoot();

                        }

                        webrtcSignalManager->webrtcSocketMap.clear();

                        closeLatch.count_down();
                    });
            }

            closeLatch.wait();

            hope::iocp::AsioProactors::getInstance()->stop();

            webrtcSignalManagers.clear();

            LOG_INFO("Already CloseBoot");

        }

        bool WebrtcSignalServer::postTask(size_t channelIndex, absl::AnyInvocable<void(std::shared_ptr<WebrtcSignalManager> &)>&& asyncHandle)
        {
            if (channelIndex >= webrtcSignalManagers.size()) {
                LOG_ERROR("Invalid ChannelIndex: {}, Size: {}", channelIndex, webrtcSignalManagers.size());
                return false;
            }

            std::shared_ptr<WebrtcSignalManager> & webrtcSignalManager = webrtcSignalManagers[channelIndex];
            if (!webrtcSignalManager) {
                LOG_ERROR("WebrtcSignalManager At Index {} Is Null", channelIndex);
                return false;
            }

            boost::asio::post(webrtcSignalManager->getLogicSystem()->getIoCompletionPorts(),
                [&webrtcSignalManager, asyncHandle = std::move(asyncHandle)]()mutable -> void {
                    asyncHandle(webrtcSignalManager);
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