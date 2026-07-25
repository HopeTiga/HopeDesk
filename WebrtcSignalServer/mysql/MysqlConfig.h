#pragma once

#include <string>
#include <cstddef>

namespace hope {

	namespace mysql {

		// MySQL 连接池配置。由 main.cpp 在启动阶段(任何线程起来之前)填充一次,
		// WebRTCMysqlManagerPools 构造时读取。
		// 为什么走全局而非构造注入:Mysql 配置要穿透
		//   main -> WebRTCSignalServer -> WebRTCSignalManager -> WebRTCLogicSystem -> WebRTCMysqlManagerPools
		// 四层,中间三层自身根本不关心 MySQL 连接参数,纯透传层数太深,
		// 按约定改用全局配置,避免把无关构造函数都污染一遍。
		struct MysqlConfig {

			std::string host{"127.0.0.1"};

			unsigned short port{3306};

			std::string username;

			std::string password;

			std::string database;

			bool multiQueries{false};

			std::size_t poolInitialSize{2};

			std::size_t poolMaxSize{16};

			int connectTimeoutSeconds{20};

			int pingIntervalSeconds{3600};

			int pingTimeoutSeconds{10};

		};

		// 全局配置实例。C++17 inline 变量,跨编译单元唯一。
		// 只在 main.cpp 启动阶段写入,之后只读,无需加锁。
		inline MysqlConfig globalMysqlConfig;

	}

}