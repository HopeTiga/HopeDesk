
#include "WebrtcSignalManager.h"

#include <chrono>

#include <boost/asio.hpp>

#include "WebrtcSignalServer.h"
#include "WebrtcSignalSocket.h"

#include "../executor/SchedulerContext.h"

#include "../utils/Utils.h"

namespace hope {

    namespace signal {

        WebrtcSignalManager::WebrtcSignalManager(size_t channelIndex, boost::asio::io_context& ioContext, WebrtcSignalServer* webrtcSignalServer, TaskChannel& taskQueues, WebrtcSignalChannelConfig channelConfig)
            : channelIndex(channelIndex)
            , ioContext(ioContext)
            , webrtcSignalServer(webrtcSignalServer)
            , hashSize(channelConfig.hashSize)
            , channelConfig(channelConfig)
#ifdef __linux__
            , acceptor(ioContext)
            , httpAcceptor(ioContext)
#endif

        {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

            boost::asio::io_context& logicIoContext = ioContext;

#else

            boost::asio::io_context& logicIoContext = hope::executor::SchedulerContext::getLogicInstance()->getIoCompletePort(channelIndex);

#endif

            hope::signal::WebrtcLogicConfig webrtcLogicConfig{ channelConfig.threshold, channelConfig.exitThreshold, channelConfig.asyncThreshold, channelConfig.mysqlConfig, channelConfig.redisConfig };

            webrtcLogicSystem = std::make_shared<hope::signal::WebrtcLogicSystem>(logicIoContext, channelIndex, taskQueues, webrtcLogicConfig);

            webrtcLogicSystem->asyncEvent();

        }

        WebrtcSignalManager::~WebrtcSignalManager()
        {
            webrtcSocketMap.clear();

        }

        std::shared_ptr<WebrtcLogicSystem> WebrtcSignalManager::getLogicSystem()
        {
            return webrtcLogicSystem;
        }

        WebrtcSignalServer* WebrtcSignalManager::getWebrtcSignalServer()
        {
            return webrtcSignalServer;
        }

        boost::asio::io_context& WebrtcSignalManager::getIoCompletionPorts() {

            return ioContext;

        }

        std::shared_ptr<hope::signal::WebrtcSignalSocket> WebrtcSignalManager::generateWebrtcSignalSocket() {

            return std::make_shared<hope::signal::WebrtcSignalSocket>(getIoCompletionPorts(), this, channelConfig.maxTlsHandShakeTime);

        }

        std::shared_ptr<HttpSocket> WebrtcSignalManager::generateHttpSocket() {

            return std::make_shared<HttpSocket>(ioContext, this, channelConfig.maxTlsHttpHandShakeTime, channelConfig.maxHttpKeepAliveTime);

        }

        void WebrtcSignalManager::registerSocket(const std::string& accountId, std::shared_ptr<WebrtcSignalSocket> webrtcSignalSocket) {

            std::string sessionId = webrtcSignalSocket->getSessionId();

            StringKeyedNodeMap<std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSocketMap.find(accountId);

            if (iterator != webrtcSocketMap.end()) {

                iterator->second->closeEvent();

            }

            webrtcSocketMap[accountId] = std::move(webrtcSignalSocket);

            int mapChannelIndex = hasher(accountId) % hashSize;

            int newChannelIndex = static_cast<int>(channelIndex);

            absl::AnyInvocable<void(WebrtcSignalManager*)> updateGlobalIndexAndKick = [accountId, sessionId = std::move(sessionId), newChannelIndex](WebrtcSignalManager* targetManager) mutable {

                StringKeyedNodeMap<WebrtcSignalManager::ActorMapping>::iterator indexIterator = targetManager->actorSocketMappingIndex.find(accountId);

                int oldChannelIndex = -1;

                std::string oldSessionId;

                if (indexIterator != targetManager->actorSocketMappingIndex.end()) {

                    oldChannelIndex = indexIterator->second.channelIndex;

                    oldSessionId = indexIterator->second.sessionId;

                }

                WebrtcSignalManager::ActorMapping actorMapping{ std::move(sessionId), newChannelIndex };

                targetManager->actorSocketMappingIndex[accountId] = std::move(actorMapping);

                if (oldChannelIndex != -1 && oldChannelIndex != newChannelIndex) {

                    targetManager->webrtcSignalServer->postTask(oldChannelIndex,
                        [accountId, oldSessionId](std::shared_ptr<WebrtcSignalManager> oldManager) mutable {

                            oldManager->removeConnection(std::move(accountId), std::move(oldSessionId), nullptr);

                        });

                }

                };

            if (mapChannelIndex == newChannelIndex) {

                updateGlobalIndexAndKick(this);

            }
            else {

                webrtcSignalServer->postTask(mapChannelIndex,
                    [managers = shared_from_this(), updateGlobalIndexAndKick = std::move(updateGlobalIndexAndKick)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager)mutable -> boost::asio::awaitable<void> {

                        updateGlobalIndexAndKick(webrtcSignalManager.get());

                        co_return;

                    });

            }

        }

        void WebrtcSignalManager::removeConnection(std::string accountId, std::string sessionId, void* nullPoint)
        {
            LOG_INFO("Remove WebrtcSignalSocket Request: Account={}, SessionId={}", accountId.c_str(), sessionId.c_str());

            StringKeyedNodeMap<std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSocketMap.find(accountId);

            if (iterator == webrtcSocketMap.end()) {
                LOG_WARN("Connection Already Removed Or Not Found: {}", accountId.c_str());
                return;
            }

            std::shared_ptr<WebrtcSignalSocket> currentSocket = iterator->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARN("Race Condition Detected! Ignore Remove Request. "
                    "Account: {}, RequestSessionId: {}, CurrentMapSessionId: {}",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return;
            }

            currentSocket->closeEvent();

            webrtcSocketMap.erase(iterator);

            int mapChannelIndex = hasher(accountId) % hashSize;

            webrtcSignalServer->postTask(mapChannelIndex, [accountId = std::move(accountId), sessionId = std::move(sessionId)](std::shared_ptr<WebrtcSignalManager> manager) -> boost::asio::awaitable<void> {

                StringKeyedNodeMap<WebrtcSignalManager::ActorMapping>::iterator iteratorIndex = manager->actorSocketMappingIndex.find(accountId);

                if (iteratorIndex != manager->actorSocketMappingIndex.end() && iteratorIndex->second.sessionId == sessionId) {

                    manager->actorSocketMappingIndex.erase(iteratorIndex);

                    LOG_INFO("Global Index Removed: {}", accountId.c_str());

                }

                co_return;

                });


        }

        void WebrtcSignalManager::removeConnection(std::string accountId, std::string sessionId)
        {
            LOG_INFO("Remove WebrtcSignalSocket Request: Account={}, SessionId={}", accountId.c_str(), sessionId.c_str());

            StringKeyedNodeMap<std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSocketMap.find(accountId);

            if (iterator == webrtcSocketMap.end()) {
                LOG_WARN("Connection Already Removed Or Not Found: {}", accountId.c_str());
                return;
            }

            std::shared_ptr<WebrtcSignalSocket> currentSocket = iterator->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARN("Race Condition Detected! Ignore Remove Request. "
                    "Account: {}, RequestSessionId: {}, CurrentMapSessionId: {}",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return;
            }

            webrtcSocketMap.erase(iterator);

            int mapChannelIndex = hasher(accountId) % hashSize;

            webrtcSignalServer->postTask(mapChannelIndex, [accountId = std::move(accountId), sessionId = std::move(sessionId)](std::shared_ptr<WebrtcSignalManager> manager) -> boost::asio::awaitable<void> {

                StringKeyedNodeMap<WebrtcSignalManager::ActorMapping>::iterator iteratorIndex = manager->actorSocketMappingIndex.find(accountId);

                if (iteratorIndex != manager->actorSocketMappingIndex.end() && iteratorIndex->second.sessionId == sessionId) {

                    manager->actorSocketMappingIndex.erase(iteratorIndex);

                    LOG_INFO("Global Index Removed: {}", accountId.c_str());

                }

                co_return;

                });


        }

        int WebrtcSignalManager::getChannelIndex()
        {
            return channelIndex;
        }

#ifdef __linux__

        void WebrtcSignalManager::asyncAccept(std::atomic<bool>& runAccepct, boost::asio::ip::tcp::endpoint endpoint, boost::asio::ip::tcp::endpoint httpEndpoint, int enableHttp)
        {

            acceptor.open(endpoint.protocol());

            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

            acceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

            acceptor.bind(endpoint);

            acceptor.listen();

            boost::asio::co_spawn(ioContext, [self = shared_from_this(), &runAccepct]() ->boost::asio::awaitable<void> {

                while (runAccepct.load()) {

                    std::shared_ptr<hope::signal::WebrtcSignalSocket> webrtcSignalSocket = std::make_shared<hope::signal::WebrtcSignalSocket>(self->ioContext, self.get(), self->channelConfig.maxTlsHandShakeTime);

                    bool shouldBackoff = false;

                    try {

                        co_await self->acceptor.async_accept(webrtcSignalSocket->getSocket(), boost::asio::use_awaitable);

                    }
                    catch (const boost::system::system_error& e) {

                        if (e.code() == boost::asio::error::operation_aborted || !runAccepct.load() || !self->acceptor.is_open()) {

                            LOG_INFO("Accept Loop Exits: {}", e.code().message().c_str());

                            break;

                        }

                        LOG_WARN("Accept Failed, Backoff and Retry: {}", e.code().message().c_str());

                        shouldBackoff = true;

                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("Accept Loop Fatal Exception: {}", e.what());

                        break;

                    }

                    if (shouldBackoff) {

                        boost::asio::steady_timer backoffTimer(self->ioContext);

                        backoffTimer.expires_after(std::chrono::milliseconds(100));

                        co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                        continue;

                    }

#ifdef HOPE_RTC_SIGNAL_SERVER_LOGIC

                    webrtcSignalSocket->setOnDisConnectHandle([webrtcSignalManager = self->shared_from_this()](std::string accountId, std::string sessionId) {

                        boost::asio::io_context& ioContext = webrtcSignalManager->getLogicSystem()->getIoCompletionPorts();

                        boost::asio::post(ioContext, [webrtcSignalManager = std::move(webrtcSignalManager), accountId = std::move(accountId), sessionId = std::move(sessionId)] {

                            webrtcSignalManager->removeConnection(std::move(accountId), std::move(sessionId));

                            });

                        });
#else

                    webrtcSignalSocket->setOnDisConnectHandle([this](std::string accountId, std::string sessionId) {

                        this->removeConnection(std::move(accountId), std::move(sessionId));

                        });
#endif


                    boost::asio::co_spawn(webrtcSignalSocket->getIoCompletionPorts(), [webrtcSignalSocket = webrtcSignalSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                        if (co_await webrtcSignalSocket->handShake()) {

                            webrtcSignalSocket->asyncEvent();

                        }

                        }, boost::asio::detached);


                }

                }, [self = shared_from_this()](std::exception_ptr ptr) {

                    if (ptr) {

                        try { std::rethrow_exception(ptr); }

                        catch (const std::exception& e) {

                            LOG_ERROR("Accept Loop Unhandled Exception: {}", e.what());

                        }

                    }

                    });

                if (enableHttp == 1) {

                    httpAcceptor.open(httpEndpoint.protocol());

                    httpAcceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

                    httpAcceptor.set_option(boost::asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>(true));

                    httpAcceptor.bind(httpEndpoint);

                    httpAcceptor.listen();

                    boost::asio::co_spawn(ioContext, [self = shared_from_this(), &runAccepct]() ->boost::asio::awaitable<void> {

                        while (runAccepct.load()) {

                            std::shared_ptr<HttpSocket> httpSocket = self->generateHttpSocket();

                            bool shouldBackoff = false;

                            try {

                                co_await self->httpAcceptor.async_accept(httpSocket->getSocket(), boost::asio::use_awaitable);

                            }
                            catch (const boost::system::system_error& e) {

                                if (e.code() == boost::asio::error::operation_aborted || !runAccepct.load() || !self->httpAcceptor.is_open()) {

                                    LOG_INFO("Accept Loop Exits: {}", e.code().message().c_str());

                                    break;

                                }

                                LOG_WARN("Accept Failed, Backoff And Retry: {}", e.code().message().c_str());

                                shouldBackoff = true;

                            }
                            catch (const std::exception& e) {

                                LOG_ERROR("Http Accept Loop Fatal Exception: {}", e.what());

                                break;

                            }

                            if (shouldBackoff) {

                                boost::asio::steady_timer backoffTimer(self->ioContext);

                                backoffTimer.expires_after(std::chrono::milliseconds(100));

                                co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                                continue;

                            }

                            boost::asio::co_spawn(httpSocket->getIoContext(), [httpSocket = httpSocket->shared_from_this()]()->boost::asio::awaitable<void> {

                                co_await httpSocket->asyncEvent();

                                co_return;

                                }, boost::asio::detached);

                        }

                        co_return;

                        }, [self = shared_from_this()](std::exception_ptr ptr) {

                            if (ptr) {

                                try { std::rethrow_exception(ptr); }

                                catch (const std::exception& e) {

                                    LOG_ERROR("Http Accept Loop Unhandled Exception: {}", e.what());

                                }

                            }

                            });

                }

        }

#endif

    }

}