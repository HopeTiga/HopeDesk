#pragma once
#define YLT_ENABLE_SSL
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <system_error>
#include <utility>
#include <filesystem>
#include <type_traits>

#include <ylt/coro_io/coro_io.hpp>
#include <ylt/coro_io/load_balancer.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>
#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_io/client_pool.hpp>

#include <async_simple/coro/SyncAwait.h>
#include <async_simple/coro/Lazy.h>

#include <boost/asio/async_result.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/post.hpp>

#include "../utils/Utils.h"   // LOG_ERROR 等日志宏
#include "CoroRpcConfig.h"

namespace hope {

	namespace rpc {

		class CoroRpc{

		public:

			static CoroRpc* getInstance() {
			
				static CoroRpc coroRpc;

				return &coroRpc;

			}

			CoroRpc(const CoroRpc& coroRpc) = delete;

			CoroRpc& operator=(const CoroRpc& coroRpc) = delete;

			bool initCoroRpc(CoroRpcServerConfig coroRpcServerConfig);

			bool asyncEvent();

			void closeEvent();

			bool isOpen();

			// 注册自由/静态协程 RPC 函数。必须在 asyncEvent() 之前调用。
			// 用法: rpc->registerHandler<echo, add>();
			template <auto... functions>
			void registerHandler() {
				coroRpcServer->register_handler<functions...>();
			}

			// 注册成员协程 RPC 函数。必须在 asyncEvent() 之前调用。
			// 用法: rpc->registerHandler<&Foo::bar, &Foo::baz>(&foo);
			template <auto first, auto... functions, typename Self>
			void registerHandler(Self* self) {
				coroRpcServer->register_handler<first, functions...>(self);
			}

			void createClientPools();

			// 创建负载均衡器。hosts 为下游服务地址列表(如 "127.0.0.1:9001");
			// weights 仅在 lba == WRR 时需要,长度需与 hosts 一致。
			// 注意:必须先 createClientPools() 成功后再调用。
			void createLoadBalancer(
				const std::vector<std::string>& hosts,
				const std::vector<int>& weights = std::vector<int>(),
				coro_io::load_balance_algorithm lba = coro_io::load_balance_algorithm::RR);

			// asyncAwait(协程函数, 参数...)：把参数以【协程参数】形式传进 Lazy，跑在 io 池上，立即返回。
			// 不要用 lambda capture 传数据：capture 走 lambda 对象，跨线程(Linux)会丢/坏，
			// 参数走协程 ABI 直接进帧，稳定。
			// 用法：
			//   coroRpc->asyncAwait([](CoroRpc* rpc, std::string data) -> async_simple::coro::Lazy<void> {
			//       // 协程体里直接用参数 data
			//       co_return;
			//   }, rpc, std::move(data));
			template <typename Func, typename... Args>
			inline void asyncAwait(Func func, Args&&... args) {
				using LazyType = std::decay_t<decltype(func(std::forward<Args>(args)...))>;
				LazyType lazy = func(std::forward<Args>(args)...);
				std::move(lazy).via(ioExecutor()).start([](async_simple::Try<typename LazyType::ValueType>&& result) {
					if (result.hasError()) {
						try {
							std::rethrow_exception(result.getException());
						}
						catch (const std::exception& e) {
							LOG_ERROR("CoroRpc::asyncAwait coroutine exception: {}", e.what());
						}
						catch (...) {
							LOG_ERROR("CoroRpc::asyncAwait coroutine unknown exception");
						}
					}
				});
			}

			// 把 Lazy 的完成交回 asio 侧。完成回调跑在 io 池线程上，必须 post 回 handler 自己关联的执行器
			// 再调 —— 那个执行器就是等待方 asio 那一帧的执行器，不 post 等于在 io 池线程上恢复别人的帧。
			template <typename T>
			class LazyAwaitInitiation {

			public:

				LazyAwaitInitiation(async_simple::coro::Lazy<T> lazy, async_simple::Executor* executor)
					: lazy(std::move(lazy)), executor(executor) {

				}

				template <typename HandlerType>
				void operator()(HandlerType&& handler) {

					std::move(lazy).via(executor).start(
						[handler = std::forward<HandlerType>(handler)](async_simple::Try<T>&& result) mutable {


							decltype(boost::asio::get_associated_executor(handler)) handlerExecutor = boost::asio::get_associated_executor(handler);

							boost::asio::post(handlerExecutor,
								[handler = std::move(handler), result = std::move(result)]() mutable {

									if (result.hasError()) {

										handler(result.getException(), T{});

										return;

									}

									handler(std::exception_ptr{}, std::move(result).value());

								});

						});

				}

			private:

				async_simple::coro::Lazy<T> lazy;

				async_simple::Executor* executor;

			};

			template <typename T>
			class LazyAwaitOperation {

			public:

				LazyAwaitOperation(async_simple::coro::Lazy<T> lazy, async_simple::Executor* executor)
					: lazy(std::move(lazy)), executor(executor) {

				}

				template <typename CompletionToken>
				typename boost::asio::async_result<std::decay_t<CompletionToken>, void(std::exception_ptr, T)>::return_type
					operator()(CompletionToken&& token) {

					LazyAwaitInitiation<T> initiation(std::move(lazy), executor);

					return boost::asio::async_initiate<std::decay_t<CompletionToken>, void(std::exception_ptr, T)>(initiation, token);

				}

			private:

				async_simple::coro::Lazy<T> lazy;

				async_simple::Executor* executor;

			};

			template <typename T>
			LazyAwaitOperation<T> asyncAwaitResult(async_simple::coro::Lazy<T> lazy) {

				return LazyAwaitOperation<T>(std::move(lazy), ioExecutor());

			}


			template <typename Op>
			auto asyncRpcRequest(std::string_view host, Op op)
				-> decltype(std::declval<coro_io::client_pools<coro_rpc::coro_rpc_client>&>()
					.send_request(host, std::move(op))) {

				using result_t = typename decltype(std::declval<
					coro_io::client_pools<coro_rpc::coro_rpc_client>&>()
					.send_request(host, std::move(op)))::ValueType;

				if (!asyncEvents.load() || !clientPools) {
					co_return result_t{ ylt::unexpect, std::errc::not_connected };
				}

				co_return co_await clientPools->send_request(host, std::move(op));
			}

			template <typename Op>
			auto asyncLbRpcRequest(Op op)
				-> decltype(std::declval<coro_io::load_balancer<coro_rpc::coro_rpc_client>&>()
					.send_request(std::move(op))) {

				using result_t = typename decltype(std::declval<
					coro_io::load_balancer<coro_rpc::coro_rpc_client>&>()
					.send_request(std::move(op)))::ValueType;

				if (!asyncEvents.load() || !loadBalancer) {
					co_return result_t{ ylt::unexpect, std::errc::not_connected };
				}

				co_return co_await loadBalancer->send_request(std::move(op));
			}

			template <auto func>
			async_simple::coro::Lazy<ylt::expected<coro_rpc::rpc_result<std::string_view>, std::errc>>
				asyncRequestRaw(std::string_view host, std::string payload) {
				if (!asyncEvents.load() || !clientPools) {
					co_return ylt::expected<coro_rpc::rpc_result<std::string_view>, std::errc>{
						ylt::unexpect, std::errc::not_connected};
				}
				co_return co_await clientPools->send_request(host,
					[payload = std::move(payload)](coro_rpc::coro_rpc_client& cli) mutable
					-> async_simple::coro::Lazy<coro_rpc::rpc_result<std::string_view>> {
					cli.set_req_attachment(std::string_view{ payload });
					coro_rpc::rpc_result<void> r = co_await cli.call<func>();  // func: void(context<void>), 无 typed 参数
					if (!r) co_return coro_rpc::rpc_result<std::string_view>{ylt::unexpect, std::move(r).error()};
					co_return coro_rpc::rpc_result<std::string_view>{cli.get_resp_attachment()};
					});
			}

			void removeHost(std::string_view host);

			void removeHosts(const std::vector<std::string>& hosts);

			void removeHostsNotIn(const std::vector<std::string>& existingHosts);

		private:

			CoroRpc();

		public:

			async_simple::Executor* ioExecutor();

			CoroRpcServerConfig coroRpcServerConfig;

		private:

			std::shared_ptr<coro_rpc::coro_rpc_server> coroRpcServer;

			std::shared_ptr<coro_io::client_pools<coro_rpc::coro_rpc_client>> clientPools;

			std::shared_ptr<coro_io::load_balancer<coro_rpc::coro_rpc_client>> loadBalancer;

			coro_rpc::coro_rpc_client::config clientConfig;

			std::atomic<bool> asyncEvents{ false };

			std::atomic<bool> initCoroRpcAtomic{ false };


		};

	}

}