#include "CoroRpc.h"

#include <iostream>
#include "../utils/Utils.h"

namespace hope {

	namespace rpc {
	
		CoroRpc::CoroRpc()
			: coroRpcServer(nullptr)
			, clientPools(nullptr)
			, loadBalancer(nullptr)
		{

		}

		bool CoroRpc::initCoroRpc(CoroRpcServerConfig coroRpcServerConfig)
		{
			if (initCoroRpcAtomic.exchange(true)) return true;

			this->coroRpcServerConfig = coroRpcServerConfig;

			coroRpcServer = std::make_shared<coro_rpc::coro_rpc_server>(
				coroRpcServerConfig.threadSize, coroRpcServerConfig.port);

			LOG_INFO("CoroRpc Constructed: Port={}, ThreadSize={}, EnableSsl={}",
				coroRpcServerConfig.port, coroRpcServerConfig.threadSize,
				coroRpcServerConfig.enableSsl ? 1 : 0);

			if (coroRpcServerConfig.enableSsl) {
				// ---------- 服务端 SSL ----------
				coro_rpc::ssl_configure sslConf;
				sslConf.base_path = coroRpcServerConfig.basePath;
				sslConf.cert_file = coroRpcServerConfig.certFile;
				sslConf.key_file = coroRpcServerConfig.keyFile;
				sslConf.ca_cert_file = coroRpcServerConfig.caCertFile;
				sslConf.enable_client_verify = coroRpcServerConfig.enableClientVerify;
				coroRpcServer->init_ssl(sslConf);

				// ---------- 客户端 SSL（单向/双向都要走 TLS）----------
				auto join = [&](const std::string& f) -> std::filesystem::path {
					return f.empty() ? std::filesystem::path{}
					: std::filesystem::path(coroRpcServerConfig.basePath).append(f);
					};

				bool hasClientCert = !coroRpcServerConfig.clientCertFile.empty();
				bool hasClientKey = !coroRpcServerConfig.clientKeyFile.empty();
				// mTLS 必须显式打开 enableDoubleSsl 才算数：
				// 单个 TLS(enableDoubleSsl=0) 时即使 config 里填了 clientCertFile/clientKeyFile
				// 也不准把证书当客户端证书加载；双向(enableDoubleSsl=1) 时才用。
				bool mtls = coroRpcServerConfig.enableDoubleSsl && hasClientCert && hasClientKey;

				// 双向认证特有的参数校验（保持原逻辑）
				if (coroRpcServerConfig.enableDoubleSsl) {
					if (hasClientCert != hasClientKey) {
						throw std::runtime_error(
							"MTLS Requires Both ClientCertFile And ClientKeyFile, "
							"Only One Is Provided, Fallback To Single TLS.\n");
					}
					if (coroRpcServerConfig.enableClientVerify && !mtls) {
						throw std::runtime_error(
							"Server EnableClientVerify=True Requires Client To Present Certificate, "
							"But ClientCertFile/ClientKeyFile Are Not Both Provided, MTLS Handshake Will Fail.\n");
					}
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

			return true;
		}

		bool CoroRpc::asyncEvent()
		{

			if (!initCoroRpcAtomic.load()) return false;

			if (asyncEvents.exchange(true)) return false;

			LOG_INFO("CoroRpcServer AsyncStart");

			coroRpcServer->async_start();

			return true;

		}

		void CoroRpc::closeEvent()
		{

			if (!asyncEvents.exchange(false)) return;

			LOG_INFO("CoroRpcServer Stop");

			coroRpcServer->stop();

		}

		bool CoroRpc::isOpen() {
		
			return asyncEvents.load();

		}

		void CoroRpc::createClientPools()
		{

			if (this->clientPools) this->clientPools = nullptr;

			LOG_INFO("CreateClientPools: Recreating ClientPools");

			coro_io::io_context_pool& ioContextPools = coroRpcServer->get_io_context_pool();

			coro_io::client_pool<coro_rpc::coro_rpc_client, coro_io::io_context_pool>::pool_config poolConfig;

			poolConfig.client_config = clientConfig;  // 带 SSL(若 enableSsl)

			this->clientPools = std::make_shared<coro_io::client_pools<coro_rpc::coro_rpc_client>>(poolConfig, ioContextPools);

		}

		void CoroRpc::createLoadBalancer(
			const std::vector<std::string>& hosts,
			const std::vector<int>& weights,
			coro_io::load_balance_algorithm lba)
		{

			if (!clientPools) return;

			LOG_INFO("CreateLoadBalancer: HostCount={}, Lba={}", hosts.size(), static_cast<int>(lba));

			std::vector<std::string_view> hostViews(hosts.begin(), hosts.end());

			typename coro_io::load_balancer<coro_rpc::coro_rpc_client>::load_balancer_config lbCfg;
			lbCfg.lba = lba;
			lbCfg.pool_config.client_config = clientConfig;

			auto lb = coro_io::load_balancer<coro_rpc::coro_rpc_client>::create(
				hostViews, lbCfg, weights, *clientPools);

			loadBalancer = std::make_shared<coro_io::load_balancer<coro_rpc::coro_rpc_client>>(std::move(lb));

		}

		async_simple::Executor* CoroRpc::ioExecutor() {
			return coroRpcServer->get_io_context_pool().get_executor();
		}

		void CoroRpc::removeHost(std::string_view host) {
			if (clientPools) {
				clientPools->erase(host);
				LOG_DEBUG("RemoveHost: Host Removed From ClientPools");
			}
		}

		void CoroRpc::removeHosts(const std::vector<std::string>& hosts) {
			if (!clientPools) return;
			for (auto& h : hosts) clientPools->erase(h);
			LOG_DEBUG("RemoveHosts: Count={}", hosts.size());
		}

		void CoroRpc::removeHostsNotIn(const std::vector<std::string>& existingHosts) {
			if (clientPools) {
				clientPools->erase_not_in(existingHosts);
				LOG_DEBUG("RemoveHostsNotIn: ExistingCount={}", existingHosts.size());
			}
		}


	}

}