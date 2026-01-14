#include "MsquicMysqlManagerPools.h"
#include "AsioProactors.h"
#include "ConfigManager.h"
#include "Utils.h"


namespace hope {

	namespace mysql {
	
		MsquicMysqlManagerPools::MsquicMysqlManagerPools(size_t size):size(size)
			, transactionChannels(std::make_shared<boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::shared_ptr<MsquicMysqlManager>)>>(ioContext, size)) {
		
			for (int i = 0; i < size; i++) {

				std::shared_ptr<MsquicMysqlManager> msquicMysqlManger =  std::make_shared<MsquicMysqlManager>(hope::iocp::AsioProactors::getInstance()->getIoCompletePorts().second);

				msquicMysqlManger->initConnection(ConfigManager::Instance().GetString("Mysql.ip")
					, ConfigManager::Instance().GetInt("Mysql.port")
					, ConfigManager::Instance().GetString("Mysql.username")
					, ConfigManager::Instance().GetString("Mysql.password")
					, ConfigManager::Instance().GetString("Mysql.database"));

				transactionMysqlManagers.enqueue(msquicMysqlManger);

				transactionChannels->async_send(boost::system::error_code{}, msquicMysqlManger,
					boost::asio::detached); 

			}

			workGuard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(ioContext.get_executor());

			ioThread = std::thread([this]() {

				ioContext.run();

				});

		}

		MsquicMysqlManagerPools::~MsquicMysqlManagerPools() {

			workGuard.reset(); 

			ioContext.stop();  

			if (ioThread.joinable()) {

				ioThread.join();

			}

		}

		boost::asio::awaitable<MsquicMysqlManagerPools::ScopedMysqlConnection> MsquicMysqlManagerPools::getTransactionMysqlManager()
		{
			std::shared_ptr<MsquicMysqlManager> msquicMysqlManager;

			try {

				msquicMysqlManager = co_await transactionChannels->async_receive(boost::asio::use_awaitable);
			}
			catch (const boost::system::system_error& e) {

				LOG_ERROR("MsquicMysqlManagerPools::getTransactionMysqlManager Error:%s", e.what());

			}
        
			co_return MsquicMysqlManagerPools::ScopedMysqlConnection(msquicMysqlManager,this);
			
		}

		void MsquicMysqlManagerPools::returnTransactionMysqlManager(std::shared_ptr<MsquicMysqlManager> mysqlManager)
		{
			if (!mysqlManager) return;

			transactionChannels->async_send(boost::system::error_code{}, std::move(mysqlManager),boost::asio::detached);

		}

	}

}
