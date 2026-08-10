#include "WebrtcSignalManager.h"

#include <chrono>

#include <boost/asio.hpp>

#include "WebrtcSignalServer.h"
#include "WebrtcSignalSocket.h"

#include "../iocp/AsioProactors.h"

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

            webrtcLogicSystem = std::make_shared<hope::signal::WebrtcLogicSystem>(ioContext, channelIndex, taskQueues, channelConfig.threshold, channelConfig.exitThreshold, channelConfig.asyncThreshold);

#else

            webrtcLogicSystem = std::make_shared<hope::signal::WebrtcLogicSystem>(hope::iocp::AsioProactors::getLogicInstance()->getIoCompletePort(channelIndex), channelIndex, taskQueues, channelConfig.threshold, channelConfig.exitThreshold, channelConfig.asyncThreshold);

#endif

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

            absl::node_hash_map<std::string, std::shared_ptr<WebrtcSignalSocket>>::iterator iterator = webrtcSocketMap.find(accountId);

            if (iterator != webrtcSocketMap.end()) {

                iterator->second->closeEvent();

            }

            webrtcSocketMap[accountId] = std::move(webrtcSignalSocket);

            int mapChannelIndex = hasher(accountId) % hashSize;

            int newChannelIndex = static_cast<int>(channelIndex);

            absl::AnyInvocable<void(WebrtcSignalManager*)> updateGlobalIndexAndKick = [accountId, sessionId = std::move(sessionId), newChannelIndex](WebrtcSignalManager* targetManager) mutable {

                absl::node_hash_map<std::string, WebrtcSignalManager::ActorMapping>::iterator indexIterator = targetManager->actorSocketMappingIndex.find(accountId);

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

                            oldManager->removeConnection(std::move(accountId), std::move(oldSessionId));

                        });

                }

                };

            if (mapChannelIndex == newChannelIndex) {

                updateGlobalIndexAndKick(this);

            }
            else {

                webrtcSignalServer->postTaskAsync(mapChannelIndex,
                    [managers = shared_from_this(), updateGlobalIndexAndKick = std::move(updateGlobalIndexAndKick)](std::shared_ptr<WebrtcSignalManager> webrtcSignalManager)mutable -> boost::asio::awaitable<void> {

                        updateGlobalIndexAndKick(webrtcSignalManager.get());

                        co_return;

                    });

            }

        }

        void WebrtcSignalManager::removeConnection(std::string accountId, std::string sessionId)
        {
            LOG_INFO("Remove WebrtcSignalSocket Request: Account=%s, SessionId=%s", accountId.c_str(), sessionId.c_str());

            auto it = webrtcSocketMap.find(accountId);

            if (it == webrtcSocketMap.end()) {
                LOG_WARN("Connection already removed or not found: %s", accountId.c_str());
                return;
            }

            std::shared_ptr<WebrtcSignalSocket> currentSocket = it->second;

            if (currentSocket->getSessionId() != sessionId) {
                LOG_WARN("Race Condition Detected! Ignore remove request. "
                    "Account: %s, RequestSessionId: %s, CurrentMapSessionId: %s",
                    accountId.c_str(), sessionId.c_str(), currentSocket->getSessionId().c_str());
                return;
            }

            webrtcSocketMap.erase(it);

            currentSocket->closeEvent();

            int mapChannelIndex = hasher(accountId) % hashSize;

            webrtcSignalServer->postTaskAsync(mapChannelIndex, [accountId = std::move(accountId), sessionId = std::move(sessionId)](std::shared_ptr<WebrtcSignalManager> manager) -> boost::asio::awaitable<void> {

                auto itIndex = manager->actorSocketMappingIndex.find(accountId);

                if (itIndex != manager->actorSocketMappingIndex.end() && itIndex->second.sessionId == sessionId) {

                    manager->actorSocketMappingIndex.erase(itIndex);

                    LOG_INFO("Global Index Removed: %s", accountId.c_str());

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

                            LOG_INFO("WebrtcSignalManager accept loop exits: %s", e.code().message().c_str());

                            break;

                        }

                        LOG_WARN("WebrtcSignalManager accept failed, backoff and retry: %s", e.code().message().c_str());

                        shouldBackoff = true;

                    }
                    catch (const std::exception& e) {

                        LOG_ERROR("WebrtcSignalManager accept loop fatal exception: %s", e.what());

                        break;

                    }

                    if (shouldBackoff) {

                        boost::asio::steady_timer backoffTimer(self->ioContext);

                        backoffTimer.expires_after(std::chrono::milliseconds(100));

                        co_await backoffTimer.async_wait(boost::asio::use_awaitable);

                        continue;

                    }

                    webrtcSignalSocket->setOnDisConnectHandle([sharedManager = self->shared_from_this()](std::string accountId, std::string sessionId) {

#ifndef HOPE_RTC_SIGNAL_SERVER_LOGIC

                        sharedManager->removeConnection(std::move(accountId), std::move(sessionId));

#else

                        boost::asio::io_context& ioContext = logicSystem->getIoCompletePort();

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

                }, [self = shared_from_this()](std::exception_ptr ptr) {

                    if (ptr) {

                        try { std::rethrow_exception(ptr); }

                        catch (const std::exception& e) {

                            LOG_ERROR("WebrtcSignalManager accept loop unhandled exception: %s", e.what());

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

                                    LOG_INFO("WebrtcSignalManager http accept loop exits: %s", e.code().message().c_str());

                                    break;

                                }

                                LOG_WARN("WebrtcSignalManager http accept failed, backoff and retry: %s", e.code().message().c_str());

                                shouldBackoff = true;

                            }
                            catch (const std::exception& e) {

                                LOG_ERROR("WebrtcSignalManager http accept loop fatal exception: %s", e.what());

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

                                    LOG_ERROR("WebrtcSignalManager http accept loop unhandled exception: %s", e.what());

                                }

                            }

                            });

                }

        }

#endif

    }

}