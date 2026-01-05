#include "MsquicMysqlManagerPools.h"
#include "AsioProactors.h"
#include "ConfigManager.h"

namespace hope {

	namespace mysql {
	
		MsquicMysqlManagerPools::MsquicMysqlManagerPools(size_t size):size(size) {
		

			for (int i = 0; i < size; i++) {

				std::shared_ptr<MsquicMysqlManager> msquicMysqlManger =  std::make_shared<MsquicMysqlManager>(hope::iocp::AsioProactors::getInstance()->getIoCompletePorts().second);

				msquicMysqlManger->initConnection(ConfigManager::Instance().GetString("Mysql.ip")
					, ConfigManager::Instance().GetInt("Mysql.port")
					, ConfigManager::Instance().GetString("Mysql.username")
					, ConfigManager::Instance().GetString("Mysql.password")
					, ConfigManager::Instance().GetString("Mysql.database"));

				transactionMysqlManagers.enqueue(std::move(msquicMysqlManger));

			}

		}

		MsquicMysqlManagerPools::~MsquicMysqlManagerPools() {

			std::shared_ptr<MsquicMysqlManager> transactionMysqlManager;

			while (transactionMysqlManagers.try_dequeue(transactionMysqlManager)) {
			
				transactionMysqlManager.reset();

			}

		}

		std::shared_ptr<MsquicMysqlManager> MsquicMysqlManagerPools::getTransactionMysqlManager()
		{
			std::shared_ptr<MsquicMysqlManager> transactionMysqlManager;

			if(transactionMysqlManagers.try_dequeue(transactionMysqlManager)) {

				if (transactionMysqlManager) {
				
					return std::move(transactionMysqlManager);

				}

			}

			return nullptr;
		}

		void MsquicMysqlManagerPools::returnTransactionMysqlManager(std::shared_ptr<MsquicMysqlManager> mysqlManager)
		{
			if (mysqlManager) {
			
				transactionMysqlManagers.enqueue(std::move(mysqlManager));

			}
		}

	}

}
