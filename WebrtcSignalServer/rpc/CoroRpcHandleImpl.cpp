#include "CoroRpcHandleImpl.h"
#include "CoroRpc.h"

#include <boost/json.hpp>

#include "../signal/WebrtcSignalServer.h"
#include "../signal/WebrtcSignalManager.h"

#include "../utils/Utils.h"

namespace hope {

	namespace rpc {

		CoroRpcHandleImpl::CoroRpcHandleImpl(hope::signal::WebrtcSignalServer& webrtcSignalServer)
			: CoroRpcHandleInterface(webrtcSignalServer) {

		}

		CoroRpcHandleImpl::~CoroRpcHandleImpl() {



		}

		void CoroRpcHandleImpl::registerRpcHandle() {

			CoroRpc::getInstance()->registerHandler<&CoroRpcHandleImpl::requestForward>(this);

		}

		async_simple::coro::Lazy<RpcForwardResponse> CoroRpcHandleImpl::requestForward(RpcForward rpcforward) {

			LOG_INFO("requestForward forwardChannel: %d", rpcforward.forwardChannel);

			LOG_INFO("requestForward forwardPacket: %s", rpcforward.forwardPacket.c_str());

			boost::json::object forwardPacketJson = boost::json::parse(rpcforward.forwardPacket).as_object();

			std::string accountId = forwardPacketJson["accountId"].as_string().data();

			std::string targetId = forwardPacketJson["targetId"].as_string().data();

			async_simple::Promise<RpcForwardResponse> promise;

			async_simple::Future<RpcForwardResponse> future = promise.getFuture();

			std::vector<std::shared_ptr<hope::signal::WebrtcSignalManager>> & webrtcSignalManagers = webrtcSignalServer.getWebrtcSignalManagers();

			std::shared_ptr<hope::signal::WebrtcSignalManager> & webrtcSignalManager = webrtcSignalManagers[rpcforward.forwardChannel];

			int mapChannelIndex = webrtcSignalManager->hasher(targetId.c_str()) % webrtcSignalManager->hashSize;

			webrtcSignalServer.postTaskAsync(mapChannelIndex, [promise = std::move(promise), forwardPacketJson = std::move(forwardPacketJson),accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<hope::signal::WebrtcSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

				absl::node_hash_map<std::string, hope::signal::WebrtcSignalManager::ActorMapping>::iterator indexIterator = webrtcSignalManager->actorSocketMappingIndex.find(targetId);

				if (indexIterator == webrtcSignalManager->actorSocketMappingIndex.end()) {

					RpcForwardResponse rpcforwardResponse{ 404,"TargetId Not Register In The WebrtcSignalServer!" };

					promise.setValue(rpcforwardResponse);

					LOG_INFO("TargetId:%s Not Register In The WebrtcSignalServer!", targetId.c_str());

					co_return;

				}

				int channelIndex = indexIterator->second.channelIndex;

				if (channelIndex == webrtcSignalManager->getChannelIndex()) {

					absl::node_hash_map<std::string, std::shared_ptr<hope::signal::WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

					if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

						std::shared_ptr<hope::signal::WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

						forwardPacketJson["state"] = 200;

						forwardPacketJson["message"] = "webrtcSignalServer forward";

						targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(forwardPacketJson)));

						LOG_INFO("RpcRequest forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), "requestForward");

						RpcForwardResponse rpcforwardResponse{ 200,"Forward Success !" };

						promise.setValue(rpcforwardResponse);

						co_return;

					}
					else {

						LOG_WARN("RpcRequest forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), "requestForward");

						RpcForwardResponse rpcforwardResponse{ 404,"TargetId Not Register In The WebrtcSignalServer!" };

						promise.setValue(rpcforwardResponse);

						co_return;

					}

				}

				webrtcSignalManager->getWebrtcSignalServer()->postTaskAsync(channelIndex, [promise = std::move(promise), forwardPacketJson = std::move(forwardPacketJson), accountId = std::move(accountId), targetId = std::move(targetId)](std::shared_ptr<hope::signal::WebrtcSignalManager> webrtcSignalManager)mutable->boost::asio::awaitable<void> {

					absl::node_hash_map<std::string, std::shared_ptr<hope::signal::WebrtcSignalSocket>>::iterator iterator = webrtcSignalManager->webrtcSocketMap.find(targetId);

					if (iterator != webrtcSignalManager->webrtcSocketMap.end()) {

						std::shared_ptr<hope::signal::WebrtcSignalSocket>& targetWebrtcSignalSocket = iterator->second;

						forwardPacketJson["state"] = 200;

						forwardPacketJson["message"] = "webrtcSignalServer forward";

						targetWebrtcSignalSocket->asyncWrite(boost::json::serialize(std::move(forwardPacketJson)));

						LOG_INFO("RpcRequest forward: %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), "requestForward");

						RpcForwardResponse rpcforwardResponse{ 200,"Forward Success !" };

						promise.setValue(rpcforwardResponse);

						co_return;

					}
					else {

						LOG_WARN("RpcRequest forward Not Found (404): %s -> %s (Request Type: %s)", accountId.c_str(), targetId.c_str(), "requestForward");

						RpcForwardResponse rpcforwardResponse{ 404,"TargetId Not Register In The WebrtcSignalServer!" };

						promise.setValue(rpcforwardResponse);

						co_return;

					}

					});

				},boost::asio::detached);

			co_return co_await std::move(future);

		}

	}

}

