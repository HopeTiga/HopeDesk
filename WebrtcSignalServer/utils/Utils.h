#pragma once
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <utility>
#include <chrono>
#include <boost/json.hpp>

#ifdef _WIN32
#include <direct.h>
#define mkdir(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---- spdlog（header-only，内置 fmt；仅本头文件引入 fmt，完整 spdlog 只在 Utils.cpp 编译）----
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#include <spdlog/fmt/fmt.h>     // 内置 fmt：{} 占位符 + 编译期格式校验
#include <spdlog/fmt/ostr.h>    // 支持带 operator<< 的自定义类型（error_code 等）

constexpr std::chrono::seconds PING_INTERVAL = std::chrono::seconds(30);

// 日志级别
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,  // 改为 WARN
    LOG_LEVEL_ERROR
} LogLevel;

// 级别开关（供宏在调用点短路，被关闭的级别连格式化都不做；x86 对齐 int 读写天然原子）
extern int consoleOutputLevels[4];
extern int logToFileEnabled;

// 日志接口
void initLogger();
void closeLogger();
void enableFileLogging(int enable);
void setLogDirectory(const char* dir);
void setConsoleOutputLevels(int debug, int info, int warn, int error);
void setFileLoggingConfig(int enable, const char* directory, int maxFileSizeMB, int fileCount);
void setLoggerAsyncConfig(int queueSize, int threadCount);

namespace hope::log {

// 已格式化消息入口，实现于 Utils.cpp（只有该 TU 编译完整 spdlog）
void logMessage(LogLevel level, const char* file, int line, const std::string& message);

// fmt 模板入口：在调用线程先完成编译期校验与格式化，再交给上面的实现
template <typename... Args>
inline void logMessage(LogLevel level, const char* file, int line,
                       fmt::format_string<Args...> format, Args&&... args) {
    logMessage(level, file, line, fmt::format(format, std::forward<Args>(args)...));
}

} // namespace hope::log

// 便捷宏定义（fmt 风格：{} 占位符，级别过滤前置：被关闭的级别连格式化都不做）
#define LOG_DEBUG(...) do { if (consoleOutputLevels[LOG_LEVEL_DEBUG] != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__); } while(0)
#define LOG_INFO(...)  do { if (consoleOutputLevels[LOG_LEVEL_INFO]  != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__); } while(0)
// warn/error 与 debug/info 同级受控：屏幕是否输出看 consoleOutputLevels，文件是否输出看 logToFileEnabled
#define LOG_WARN(...)  do { if (consoleOutputLevels[LOG_LEVEL_WARN]  != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__); } while(0)
#define LOG_ERROR(...) do { if (consoleOutputLevels[LOG_LEVEL_ERROR] != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__); } while(0)

#define LOG_INFO_PLAIN(...)  LOG_INFO(__VA_ARGS__)
#define LOG_WARN_PLAIN(...)  LOG_WARN(__VA_ARGS__)
#define LOG_ERROR_PLAIN(...) LOG_ERROR(__VA_ARGS__)
#define LOG_DEBUG_PLAIN(...) LOG_DEBUG(__VA_ARGS__)

#endif // UTILS_H
