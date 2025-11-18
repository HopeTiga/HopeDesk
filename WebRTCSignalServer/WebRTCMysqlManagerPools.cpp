#include "WebRTCMysqlManagerPools.h"
#include "AsioProactors.h"
#include "ConfigManager.h"

namespace hope {

	namespace core {
	
		WebRTCMysqlManagerPools::WebRTCMysqlManagerPools(size_t size):size(size) {
		
			for(int i = 0; i < size ; i++){
			
				mysqlManagers.emplace_back(std::make_shared<WebRTCMysqlManager>(AsioProactors::getInstance()->getIoCompletePorts().second));

				mysqlManagers[i]->initConnection(ConfigManager::Instance().GetString("Mysql.ip")
					, ConfigManager::Instance().GetInt("Mysql.port")
					, ConfigManager::Instance().GetString("Mysql.username")
					, ConfigManager::Instance().GetString("Mysql.password")
					, ConfigManager::Instance().GetString("Mysql.database"));

			}

			for (int i = 0; i < size / 2; i++) {

				std::shared_ptr<WebRTCMysqlManager> webrtcMysqlManger =  std::make_shared<WebRTCMysqlManager>(AsioProactors::getInstance()->getIoCompletePorts().second);

				webrtcMysqlManger->initConnection(ConfigManager::Instance().GetString("Mysql.ip")
					, ConfigManager::Instance().GetInt("Mysql.port")
					, ConfigManager::Instance().GetString("Mysql.username")
					, ConfigManager::Instance().GetString("Mysql.password")
					, ConfigManager::Instance().GetString("Mysql.database"));

				transactionMysqlManagers.enqueue(std::move(webrtcMysqlManger));

			}

		}

		WebRTCMysqlManagerPools::~WebRTCMysqlManagerPools() {

			mysqlManagers.clear();

			std::shared_ptr<WebRTCMysqlManager> transactionMysqlManager;

			while (transactionMysqlManagers.try_dequeue(transactionMysqlManager)) {
			
				transactionMysqlManager.reset();

			}

		}

		std::shared_ptr<WebRTCMysqlManager> WebRTCMysqlManagerPools::getMysqlManager()
		{
			size_t index = loadBalancing.fetch_add(1) % size;

			return mysqlManagers[index];

		}

		std::shared_ptr<WebRTCMysqlManager> WebRTCMysqlManagerPools::getTransactionMysqlManager()
		{
			std::shared_ptr<WebRTCMysqlManager> transactionMysqlManager;

			if(transactionMysqlManagers.try_dequeue(transactionMysqlManager)) {

				if (transactionMysqlManager) {
				
					return std::move(transactionMysqlManager);

				}

			}

			return nullptr;
		}

		void WebRTCMysqlManagerPools::returnTransactionMysqlManager(std::shared_ptr<WebRTCMysqlManager> mysqlManager)
		{
			if (mysqlManager) {
			
				transactionMysqlManagers.enqueue(std::move(mysqlManager));

			}
		}

	}

}
