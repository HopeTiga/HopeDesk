
#include "CoroRpcHandleImpl.h"
#include "CoroRpc.h"

#include <boost/json.hpp>

#include <ylt/coro_io/coro_io.hpp>
#include <ylt/struct_pack.hpp>

#include "../signal/WebrtcSignalServer.h"
#include "../signal/WebrtcSignalManager.h"
#include "../signal/WebrtcSignalPacket.h"

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

			LOG_INFO("RequestForward ForwardChannel: {}", rpcforward.forwardChannel);

			LOG_INFO("RequestForward ForwardPacket: {}", rpcforward.forwardPacket.c_str());

			boost::json::object forwardPacketJson = boost::json::parse(rpcforward.forwardPacket).as_object();

			std::string accountId = forwardPacketJson["accountId"].as_string().data();

			std::string targetId = forwardPacketJson["targetId"].as_string().data();

			std::vector<std::shared_ptr<hope::signal::WebrtcSignalManager>> & webrtcSignalManagers = webrtcSignalServer.getWebrtcSignalManagers();

			bool invalidForwardChannel = rpcforward.forwardChannel < 0
				|| rpcforward.forwardChannel >= static_cast<int>(webrtcSignalManagers.size())
				|| !webrtcSignalManagers[rpcforward.forwardChannel];

			if (invalidForwardChannel) {

				LOG_ERROR("RequestForward Invalid ForwardChannel: {}, Size: {}", rpcforward.forwardChannel, webrtcSignalManagers.size());

				co_return RpcForwardResponse{ 404, "TargetId Not Register In The WebrtcSignalServer!" };

			}

			std::shared_ptr<hope::signal::WebrtcSignalManager> & webrtcSignalManager = webrtcSignalManagers[rpcforward.forwardChannel];

			int mapChannelIndex = static_cast<int>(webrtcSignalManager->hasher(targetId.c_str()) % webrtcSignalManager->hashSize);

			if (static_cast<size_t>(mapChannelIndex) >= webrtcSignalManagers.size()) {

				LOG_ERROR("RequestForward Invalid MapChannelIndex: {}, Size: {}", mapChannelIndex, webrtcSignalManagers.size());

				co_return RpcForwardResponse{ 500, "TargetId Not Register In The WebrtcSignalServer!" };

			}

			// 要转发的报文只在这里拼一次，两跳各自带一份走。
			hope::signal::WebrtcEnvelopeView webrtcResponse;

			webrtcResponse.state = 200;

			webrtcResponse.message = "WebrtcSignalServer Forward";

			if (const boost::json::value* requestTypeValue = forwardPacketJson.if_contains("requestType")) {
				if (requestTypeValue->is_int64()) {
					webrtcResponse.requestType = static_cast<int>(requestTypeValue->as_int64());
				}
			}

			webrtcResponse.accountId = accountId;

			webrtcResponse.targetId = targetId;

			forwardPacketJson.erase("requestType");

			forwardPacketJson.erase("accountId");

			forwardPacketJson.erase("targetId");

			std::string forwardMessage = struct_pack::serialize<std::string>(webrtcResponse);

			forwardMessage.append(boost::json::serialize(forwardPacketJson));

			struct ForwardLookup {

				RpcForwardResponse response;

				int channelIndex = 0;

				bool delivered = false;

			};

			std::shared_ptr<hope::signal::WebrtcSignalManager> & mapChannelManager = webrtcSignalManagers[mapChannelIndex];

			boost::asio::io_context & mapChannelIoContext = mapChannelManager->getLogicSystem()->getIoCompletionPorts();

			coro_io::callback_awaitor<int> callbackAwaitor;

			int awaitorInt = co_await callbackAwaitor.await_resume([&mapChannelIoContext,&callbackAwaitor](auto handler)mutable {
				
				boost::asio::co_spawn(mapChannelIoContext, [handler = std::move(handler)]()mutable -> boost::asio::awaitable<void> {

					LOG_INFO("callbackAwaitor");

					handler.set_value_then_resume(1);

					co_return;

					}, CompletionHandle{});

				});

			coro_io::callback_awaitor<async_simple::Try<ForwardLookup>> awaitor;

			async_simple::Try<ForwardLookup> lookup = co_await coro_io::post(
				[mapChannelManager, accountId, targetId, forwardMessage]() -> ForwardLookup {

					StringKeyedNodeMap<hope::signal::WebrtcSignalManager::ActorMapping>::iterator indexIterator = mapChannelManager->actorSocketMappingIndex.find(targetId);

					if (indexIterator == mapChannelManager->actorSocketMappingIndex.end()) {

						LOG_INFO("TargetId:{} Not Register In The WebrtcSignalServer!", targetId.c_str());

						return ForwardLookup{ RpcForwardResponse{ 404, "TargetId Not Register In The WebrtcSignalServer!" }, 0, true };

					}

					int channelIndex = indexIterator->second.channelIndex;

					if (channelIndex != mapChannelManager->getChannelIndex()) {

						return ForwardLookup{ RpcForwardResponse{}, channelIndex, false };

					}

					StringKeyedNodeMap<std::shared_ptr<hope::signal::WebrtcSignalSocket>>::iterator socketIterator = mapChannelManager->webrtcSocketMap.find(targetId);

					if (socketIterator == mapChannelManager->webrtcSocketMap.end()) {

						LOG_WARN("RpcRequest Forward Not Found (404): {} -> {} (Request Type: {})", accountId.c_str(), targetId.c_str(), "RequestForward");

						return ForwardLookup{ RpcForwardResponse{ 404, "TargetId Not Register In The WebrtcSignalServer!" }, 0, true };

					}

					socketIterator->second->asyncWrite(forwardMessage);

					LOG_INFO("RpcRequest Forward: {} -> {} (Request Type: {})", accountId.c_str(), targetId.c_str(), "RequestForward");

					return ForwardLookup{ RpcForwardResponse{ 200, "Forward Success !" }, 0, true };

				}, mapChannelIoContext.get_executor());

			if (lookup.hasError()) {

				LOG_ERROR("RequestForward Lookup Failed");

				co_return RpcForwardResponse{ 500, "TargetId Not Register In The WebrtcSignalServer!" };

			}

			ForwardLookup lookupResult = std::move(lookup).value();

			if (lookupResult.delivered) {

				co_return std::move(lookupResult.response);

			}

			if (lookupResult.channelIndex < 0 || static_cast<size_t>(lookupResult.channelIndex) >= webrtcSignalManagers.size()) {

				LOG_ERROR("RequestForward Invalid Target ChannelIndex: {}, Size: {}", lookupResult.channelIndex, webrtcSignalManagers.size());

				co_return RpcForwardResponse{ 500, "TargetId Not Register In The WebrtcSignalServer!" };

			}

			// 第二跳：投到目标所在的那条通道上。
			std::shared_ptr<hope::signal::WebrtcSignalManager> & targetChannelManager = webrtcSignalManagers[lookupResult.channelIndex];

			boost::asio::io_context & targetChannelIoContext = targetChannelManager->getLogicSystem()->getIoCompletionPorts();

			async_simple::Try<RpcForwardResponse> delivered = co_await coro_io::post(
				[targetChannelManager, accountId, targetId, forwardMessage = std::move(forwardMessage)]() mutable -> RpcForwardResponse {

					StringKeyedNodeMap<std::shared_ptr<hope::signal::WebrtcSignalSocket>>::iterator socketIterator = targetChannelManager->webrtcSocketMap.find(targetId);

					if (socketIterator == targetChannelManager->webrtcSocketMap.end()) {

						LOG_WARN("RpcRequest Forward Not Found (404): {} -> {} (Request Type: {})", accountId.c_str(), targetId.c_str(), "RequestForward");

						return RpcForwardResponse{ 404, "TargetId Not Register In The WebrtcSignalServer!" };

					}

					socketIterator->second->asyncWrite(std::move(forwardMessage));

					LOG_INFO("RpcRequest Forward: {} -> {} (Request Type: {})", accountId.c_str(), targetId.c_str(), "RequestForward");

					return RpcForwardResponse{ 200, "Forward Success !" };

				}, targetChannelIoContext.get_executor());

			if (delivered.hasError()) {

				LOG_ERROR("RequestForward Deliver Failed");

				co_return RpcForwardResponse{ 500, "TargetId Not Register In The WebrtcSignalServer!" };

			}

			co_return std::move(delivered).value();

		}

	}

}
