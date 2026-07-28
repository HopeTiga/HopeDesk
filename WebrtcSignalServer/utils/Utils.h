#pragma once
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <chrono>
#include <boost/json.hpp>

#ifdef _WIN32
#include <direct.h>
#define mkdir(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

constexpr std::chrono::seconds PING_INTERVAL = std::chrono::seconds(30);

// 日志级别
typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,  // 改为 WARN
    LOG_LEVEL_ERROR
} LogLevel;

// 级别开关（普通 int，供宏在调用点短路，避免 va_list 构造与函数调用开销）
// 读写在 x86 上对齐 int 读写天然原子，日志过滤不需要强顺序保证，用普通 int 最快。
extern int consoleOutputLevels[4];
extern int logToFileEnabled;

#ifdef __cplusplus
extern "C" {
#endif

    // 初始化函数
    void initLogger();
    void closeLogger();
    void enableFileLogging(int enable);
    void setLogDirectory(const char* dir);
    void setConsoleOutputLevels(int debug, int info, int warn, int error);

    // 核心日志函数
    void logMessage(LogLevel level, const char* file, int line, const char* format, ...);
    void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...);
    void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...);

    // 辅助函数
    void getTimestamp(char* buffer, size_t size);
    void getLevelInfo(LogLevel level, const char** levelStr, const char** color);

#ifdef __cplusplus
}
#endif

// 便捷宏定义（级别过滤前置：被关闭的级别连 va_list 都不构造）
#define LOG_INFO(fmt, ...)    do { if (consoleOutputLevels[LOG_LEVEL_INFO] != 0 || logToFileEnabled != 0) logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN(fmt, ...)    do { if (consoleOutputLevels[LOG_LEVEL_WARN] != 0 || logToFileEnabled != 0) logMessage(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...)   do { if (consoleOutputLevels[LOG_LEVEL_ERROR] != 0 || logToFileEnabled != 0) logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)   do { if (consoleOutputLevels[LOG_LEVEL_DEBUG] != 0 || logToFileEnabled != 0) logMessage(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#define LOG_INFO_PLAIN(fmt, ...)    do { if (consoleOutputLevels[LOG_LEVEL_INFO] != 0 || logToFileEnabled != 0) logMessagePlain(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_WARN_PLAIN(fmt, ...)    do { if (consoleOutputLevels[LOG_LEVEL_WARN] != 0 || logToFileEnabled != 0) logMessagePlain(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_ERROR_PLAIN(fmt, ...)   do { if (consoleOutputLevels[LOG_LEVEL_ERROR] != 0 || logToFileEnabled != 0) logMessagePlain(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG_PLAIN(fmt, ...)   do { if (consoleOutputLevels[LOG_LEVEL_DEBUG] != 0 || logToFileEnabled != 0) logMessagePlain(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); } while(0)

#endif // UTILS_H