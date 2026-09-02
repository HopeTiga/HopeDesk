#pragma once
#ifndef MIMALLOC_CONFIG_H
#define MIMALLOC_CONFIG_H

#include <mimalloc/mimalloc.h>

#include "ConfigManager.h"

namespace hope {
namespace utils {

// mimalloc 运行时配置(编译期注入,等价 Windows 侧 MIMALLOC_* 环境变量,编进产物无需运行时设置)
// 默认值在此(默认配置);config.ini 的 [Mimalloc] 段可覆盖(可变配置),未写则用默认值
struct MimallocConfig {

    int purgeDelayMs = 1000;   // MIMALLOC_PURGE_DELAY
    int purgeDecommits = 1;    // MIMALLOC_PURGE_DECOMMITS
    int destroyOnExit = 0;     // MIMALLOC_DESTROY_ON_EXIT(退出期销毁堆有崩溃风险,mimalloc 自己标注 unsafe,保持 0,OS 自动回收)
    int showStats = 0;         // MIMALLOC_SHOW_STATS(退出期打印统计要经过 CRT printf + 重定向 malloc,退出阶段会崩,保持 0)
    int verbose = 1;           // MIMALLOC_VERBOSE

};

inline void loadMimallocConfig(MimallocConfig& config, const ConfigManager& configManager) {

    config.purgeDelayMs = configManager.GetInt("Mimalloc.purgeDelayMs", config.purgeDelayMs);
    config.purgeDecommits = configManager.GetInt("Mimalloc.purgeDecommits", config.purgeDecommits);
    config.destroyOnExit = configManager.GetInt("Mimalloc.destroyOnExit", config.destroyOnExit);
    config.showStats = configManager.GetInt("Mimalloc.showStats", config.showStats);
    config.verbose = configManager.GetInt("Mimalloc.verbose", config.verbose);

}

inline void applyMimallocConfig(const MimallocConfig& config) {

    mi_option_set(mi_option_purge_delay, config.purgeDelayMs);
    mi_option_set(mi_option_purge_decommits, config.purgeDecommits);
    mi_option_set(mi_option_destroy_on_exit, config.destroyOnExit);
    mi_option_set(mi_option_show_stats, config.showStats);
    mi_option_set(mi_option_verbose, config.verbose);

}

} // namespace utils
} // namespace hope

#endif // MIMALLOC_CONFIG_H
