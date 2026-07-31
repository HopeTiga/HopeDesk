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

#include <ylt/coro_io/coro_io.hpp>
#include <ylt/coro_io/load_balancer.hpp>
#include <ylt/coro_rpc/coro_rpc_server.hpp>
#include <ylt/coro_rpc/coro_rpc_client.hpp>
#include <ylt/coro_io/client_pool.hpp>

#include <async_simple/coro/SyncAwait.h>
#include <async_simple/coro/Lazy.h>

namespace hope {

	namespace rpc {
	
		struct CoroRpcServerConfig {

			size_t port;

			size_t threadSize;

			bool enableSsl;

			std::string basePath;

			std::string certFile;

			std::string keyFile;

			std::string caCertFile; // 单向认证为空

			bool enableClientVerify = false;

			bool enableDoubleSsl = false; // 双向认证

			std::string clientCertFile;

			std::string clientKeyFile;

		};

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

			template <typename LazyType>
			inline auto asyncAwait(LazyType&& lazy) {
				// syncAwait 的异步(非阻塞)版:把 Lazy 扔到 io 池异步跑,立即返回。
				// 你在传进来的 Lazy("异步回调")里照常 co_await rpc->asyncRpcRequest(...) 用 rpc,
				// 用法跟 syncAwait 包一个 lambda 完全一样,只是不阻塞调用方。
				std::move(lazy).via(ioExecutor()).start([](auto&&) {});
			}


			// 指定 host 版本:把请求发到指定下游地址,host 形如 "ip:port"。
		// op 由调用方提供,内部就是 coro_rpc_client::call<func>(args...),形如:
		//   [](coro_rpc::coro_rpc_client& cli)
		//       -> async_simple::coro::Lazy<coro_rpc::rpc_result<R>> {
		//       co_return co_await cli.call<echo>("hi");
		//   }
		// 返回 Lazy<expected<rpc_result<R>, std::errc>>:
		//   外层 std::errc           -> 连接层错误(池未就绪 / host 不通)
		//   内层 coro_rpc::rpc_error  -> RPC 业务层错误
		// 返回类型完全由库的 client_pools::send_request 推导,不自己算 R,
		// 也不用偏特化萃取(MSVC 对那套解析有问题)。Lazy<T>::ValueType 直接就是内层 T。
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

			// 负载均衡版本:用 createLoadBalancer 配置好的 host 列表轮询 / 随机分发。
			// op 比 asyncRpcRequest 多一个 std::string_view host 参数(告诉你这次落到哪台):
			//   [](coro_rpc::coro_rpc_client& cli, std::string_view host)
			//       -> async_simple::coro::Lazy<coro_rpc::rpc_result<R>> {
			//       co_return co_await cli.call<echo>("hi");
			//   }
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

			// 原始字节请求:走 coro_rpc 的 attachment 机制(直接传字节的正路,不是 call<func>(string))。
			// func 是服务端 handler,必须是 void(coro_rpc::context<void>):用 release_request_attachment()
			// 取请求字节,set_response_attachment() 回字节,最后 response_msg()。
			// 调用方传 payload(请求字节),返回 Lazy<expected<rpc_result<string_view>, std::errc>>:
			//   外层 std::errc       -> 连接层错误(池未就绪 / host 不通)
			//   内层 rpc_error        -> RPC 业务层错误
			//   内层 value(string_view) -> 返回字节(指向 client 响应缓冲,拿到后立即用,别存)
			// 用法:co_await rpc.asyncRequestRaw<echoRaw>("127.0.0.1:10087", payload);
			template <auto func>
			async_simple::coro::Lazy<ylt::expected<coro_rpc::rpc_result<std::string_view>, std::errc>>
				asyncRequestRaw(std::string_view host, std::string payload) {
				auto op = [payload = std::move(payload)](coro_rpc::coro_rpc_client& cli) mutable
					-> async_simple::coro::Lazy<coro_rpc::rpc_result<std::string_view>> {
					cli.set_req_attachment(std::string_view{ payload });
					auto r = co_await cli.call<func>();  // func: void(context<void>), 无 typed 参数
					if (!r) co_return coro_rpc::rpc_result<std::string_view>{ylt::unexpect, std::move(r).error()};
					co_return coro_rpc::rpc_result<std::string_view>{cli.get_resp_attachment()};
					};
				if (!asyncEvents.load() || !clientPools) {
					co_return ylt::expected<coro_rpc::rpc_result<std::string_view>, std::errc>{
						ylt::unexpect, std::errc::not_connected};
				}
				co_return co_await clientPools->send_request(host, std::move(op));
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