#pragma once

#include <cstddef>

#include "../utils/ConfigManager.h"

namespace hope {

	namespace executor {

		struct SchedulerConfig {

			std::size_t threadSize{0};                  // 每个池的线程数(= 通道数),0 = 取硬件并发数

			bool ioEnableCpuAffinity{false};

			std::size_t ioCpuAffinityOffset{0};

			bool logicEnableCpuAffinity{false};

			std::size_t logicCpuAffinityOffset{0};

		};

		inline void loadSchedulerConfig(SchedulerConfig& schedulerConfig, const hope::utils::ConfigManager& configManager) {

			schedulerConfig.threadSize = configManager.GetSize("WebrtcSignalServer.threadSize");

			schedulerConfig.ioEnableCpuAffinity = configManager.GetBool("WebrtcSignalServer.ioEnableCpuAffinity", schedulerConfig.ioEnableCpuAffinity);

			// 不能用 GetSize:它把 <= 0 当"回退到核数"
			int ioCpuAffinityOffset = configManager.GetInt("WebrtcSignalServer.ioCpuAffinityOffset", 0);
			schedulerConfig.ioCpuAffinityOffset = (ioCpuAffinityOffset > 0) ? static_cast<std::size_t>(ioCpuAffinityOffset) : 0;

			schedulerConfig.logicEnableCpuAffinity = configManager.GetBool("WebrtcSignalServer.logicEnableCpuAffinity", schedulerConfig.logicEnableCpuAffinity);

			int logicCpuAffinityOffset = configManager.GetInt("WebrtcSignalServer.logicCpuAffinityOffset", 0);
			schedulerConfig.logicCpuAffinityOffset = (logicCpuAffinityOffset > 0) ? static_cast<std::size_t>(logicCpuAffinityOffset) : 0;

		}

	}

}
