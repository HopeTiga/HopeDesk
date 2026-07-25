#include "CoroRpc.h"

#include <iostream>
#include "../utils/Utils.h"

namespace hope {

	namespace rpc {
	
		CoroRpc::CoroRpc(CoroRpcServerConfig coroRpcServerConfig)
			: coroRpcServer(coroRpcServerConfig.threadSize, coroRpcServerConfig.port)
			, clientPools(nullptr)
		{

			LOG_INFO("CoroRpc constructed: port=%zu, threadSize=%zu, enableSsl=%d",
				coroRpcServerConfig.port, coroRpcServerConfig.threadSize,
				coroRpcServerConfig.enableSsl ? 1 : 0);

			if (coroRpcServerConfig.enableSsl) {

				coro_rpc::ssl_configure sslConf;

				sslConf.base_path = coroRpcServerConfig.basePath;

				sslConf.cert_file = coroRpcServerConfig.certFile;

				sslConf.key_file = coroRpcServerConfig.keyFile;

				sslConf.ca_cert_file = coroRpcServerConfig.caCertFile;

				sslConf.enable_client_verify = coroRpcServerConfig.enableClientVerify;

				coroRpcServer.init_ssl(sslConf);

				if (coroRpcServerConfig.enableDoubleSsl) {

					auto join = [&](const std::string& f) -> std::filesystem::path {

						return f.empty() ? std::filesystem::path{}

						: std::filesystem::path(coroRpcServerConfig.basePath).append(f);

						};
					bool hasClientCert = !coroRpcServerConfig.clientCertFile.empty();

					bool hasClientKey = !coroRpcServerConfig.clientKeyFile.empty();

					bool mtls = hasClientCert && hasClientKey;  // 双向

					if (hasClientCert != hasClientKey) {

						throw std::runtime_error("[CoroRpc] mTLS 需同时提供 clientCertFile 和 clientKeyFile,现只填了一个,按单向 TLS 处理。\n");

					}
					if (coroRpcServerConfig.enableClientVerify && !mtls) {

						throw std::runtime_error("[CoroRpc] 服务端 enableClientVerify=true 要求客户端出示证书,但未同时提供 clientCertFile/clientKeyFile,mTLS 握手会失败。\n");

					}
					clientConfig.socket_config =
						coro_rpc::coro_rpc_client::tcp_with_ssl_config{
						/*enableTcpNoDelay*/ true,
						/*sslCertPath(CA 验服务端)*/ join(coroRpcServerConfig.caCertFile),
						/*sslDomain*/ std::string{},  // 空 -> 127.0.0.1/localhost 跳过主机名校验
						/*clientCertFile(mTLS)*/ mtls ? join(coroRpcServerConfig.clientCertFile)
													  : std::filesystem::path{},
						/*clientKeyFile(mTLS)*/  mtls ? join(coroRpcServerConfig.clientKeyFile)
													  : std::filesystem::path{},
					};
				}

			}

		}

		void CoroRpc::asyncEvent()
		{

			if (asyncEvents.exchange(true)) return;

			LOG_INFO("CoroRpc asyncEvent: coroRpcServer asyncStart");

			coroRpcServer.async_start();

		}

		void CoroRpc::closeEvent()
		{

			if (!asyncEvents.exchange(false)) return;

			LOG_INFO("CoroRpc closeEvent: coroRpcServer stop");

			coroRpcServer.stop();

		}

		void CoroRpc::createClientPools()
		{

			if (this->clientPools) this->clientPools = nullptr;

			LOG_INFO("CoroRpc createClientPools: recreating clientPools");

			coro_io::io_context_pool& ioContextPools = coroRpcServer.get_io_context_pool();

			coro_io::client_pool<coro_rpc::coro_rpc_client, coro_io::io_context_pool>::pool_config poolConfig;

			poolConfig.client_config = clientConfig;  // 带 SSL(若 enableSsl)

			this->clientPools = std::make_shared<coro_io::client_pools<coro_rpc::coro_rpc_client>>(poolConfig, ioContextPools);

		}

		void CoroRpc::createLoadBalancer(
			const std::vector<std::string>& hosts,
			const std::vector<int>& weights,
			coro_io::load_balance_algorithm lba)
		{

			if (!clientPools || hosts.empty()) return;

			LOG_INFO("CoroRpc createLoadBalancer: hostCount=%zu, lba=%d", hosts.size(), static_cast<int>(lba));

			std::vector<std::string_view> hostViews(hosts.begin(), hosts.end());

			typename coro_io::load_balancer<coro_rpc::coro_rpc_client>::load_balancer_config lbCfg;
			lbCfg.lba = lba;
			lbCfg.pool_config.client_config = clientConfig;

			auto lb = coro_io::load_balancer<coro_rpc::coro_rpc_client>::create(
				hostViews, lbCfg, weights, *clientPools);

			loadBalancer = std::make_shared<coro_io::load_balancer<coro_rpc::coro_rpc_client>>(std::move(lb));

		}

		async_simple::Executor* CoroRpc::ioExecutor() {
			return coroRpcServer.get_io_context_pool().get_executor();
		}

		void CoroRpc::removeHost(std::string_view host) {
			if (clientPools) {
				clientPools->erase(host);
				LOG_DEBUG("CoroRpc removeHost: host removed from client_pools");
			}
		}

		void CoroRpc::removeHosts(const std::vector<std::string>& hosts) {
			if (!clientPools) return;
			for (auto& h : hosts) clientPools->erase(h);
			LOG_DEBUG("CoroRpc removeHosts: count=%zu", hosts.size());
		}

		void CoroRpc::removeHostsNotIn(const std::vector<std::string>& existingHosts) {
			if (clientPools) {
				clientPools->erase_not_in(existingHosts);
				LOG_DEBUG("CoroRpc removeHostsNotIn: existingCount=%zu", existingHosts.size());
			}
		}


	}

}