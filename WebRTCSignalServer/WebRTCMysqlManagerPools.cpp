#include "WebRTCMysqlManagerPools.h"
#include "AsioProactors.h"
#include "ConfigManager.h"
#include "Utils.h"


namespace hope {

	namespace mysql {

		WebRTCMysqlManagerPools::WebRTCMysqlManagerPools(boost::asio::io_context& ioContext, size_t size)
			: size(size)
			, ioContext(ioContext)
			, transactionChannels(std::make_shared<boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::shared_ptr<WebRTCMysqlManager>)>>(ioContext, size)) {

			for (int i = 0; i < size; i++) {

				std::shared_ptr<WebRTCMysqlManager> webrtcMysqlManager = std::make_shared<WebRTCMysqlManager>(ioContext);

				webrtcMysqlManager->initConnection(ConfigManager::Instance().GetString("Mysql.ip")
					, ConfigManager::Instance().GetInt("Mysql.port")
					, ConfigManager::Instance().GetString("Mysql.username")
					, ConfigManager::Instance().GetString("Mysql.password")
					, ConfigManager::Instance().GetString("Mysql.database"));

				transactionMysqlManagers.enqueue(webrtcMysqlManager);

				transactionChannels->async_send(boost::system::error_code{}, webrtcMysqlManager,
					boost::asio::detached);

			}

		}

		WebRTCMysqlManagerPools::~WebRTCMysqlManagerPools() {

			std::shared_ptr<WebRTCMysqlManager> webrtcMysqlManager;

			while (transactionMysqlManagers.try_dequeue(webrtcMysqlManager)) {

				webrtcMysqlManager = nullptr;

			}

		}

		boost::asio::awaitable<WebRTCMysqlManagerPools::ScopedMysqlConnection> WebRTCMysqlManagerPools::getTransactionMysqlManager()
		{
			std::shared_ptr<WebRTCMysqlManager> webrtcMysqlManager;

			try {

				webrtcMysqlManager = co_await transactionChannels->async_receive(boost::asio::use_awaitable);

				if (!webrtcMysqlManager->getConnetionStatus()) {

					if (!co_await webrtcMysqlManager->checkAndReconnect()) {

						LOG_WARNING("WebRTCMysqlManagerPools::getTransactionMysqlManager: Reconnected to MySQL DisConnected");

					}

				}
			}
			catch (const boost::system::system_error& e) {

				LOG_ERROR("WebRTCMysqlManagerPools::getTransactionMysqlManager Error:%s", e.what());

			}

			co_return WebRTCMysqlManagerPools::ScopedMysqlConnection(webrtcMysqlManager, this);

		}

		void WebRTCMysqlManagerPools::returnTransactionMysqlManager(std::shared_ptr<WebRTCMysqlManager> mysqlManager)
		{
			if (!mysqlManager) return;

			transactionChannels->async_send(boost::system::error_code{}, std::move(mysqlManager), boost::asio::detached);

		}

	}

}
