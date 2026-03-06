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

#ifdef __cplusplus
extern "C" {
#endif

    // 日志级别
    typedef enum {
        LOG_LEVEL_DEBUG,
        LOG_LEVEL_INFO,
        LOG_LEVEL_WARNING,
        LOG_LEVEL_ERROR
    } LogLevel;

    // 初始化函数
    void initLogger();
    void closeLogger();
    void enableFileLogging(int enable);
    void setLogDirectory(const char* dir);
    void setConsoleOutputLevels(int debug, int info, int warning, int error);

    // 核心日志函数
    void logMessage(LogLevel level, const char* format, ...);
    void logMessagePlain(LogLevel level, const char* format, ...);
    void logToFileOnly(LogLevel level, const char* format, ...);

    // 辅助函数
    void getTimestamp(char* buffer, size_t size);
    void getLevelInfo(LogLevel level, const char** levelStr, const char** color);

    // 便捷宏定义
#define LOG_INFO(fmt, ...)    logMessage(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logMessage(LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   logMessage(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   logMessage(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#define LOG_INFO_PLAIN(fmt, ...)    logMessagePlain(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARNING_PLAIN(fmt, ...) logMessagePlain(LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR_PLAIN(fmt, ...)   logMessagePlain(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_PLAIN(fmt, ...)   logMessagePlain(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

// 粗爆转义 JSON 字符串值（只处理 string 类型）
static void brutalEscapeJson(boost::json::object& obj) {
    for (auto& [k, v] : obj) {
        if (v.is_string()) {
            std::string s = v.as_string().c_str();
            // 只干两件事：去 \0 和 '
            std::replace(s.begin(), s.end(), '\0', ' ');
            std::replace(s.begin(), s.end(), '\'', ' ');
            obj[k] = std::move(s);
        }
        else if (v.is_object()) {
            brutalEscapeJson(v.as_object());   // 递归
        }
        else if (v.is_array()) {
            for (auto& elem : v.as_array()) {
                if (elem.is_object()) brutalEscapeJson(elem.as_object());
                if (elem.is_string()) {
                    std::string s = elem.as_string().c_str();
                    std::replace(s.begin(), s.end(), '\0', ' ');
                    std::replace(s.begin(), s.end(), '\'', ' ');
                    elem = std::move(s);
                }
            }
        }
    }
}

// 深度拷贝一份 JSON 并转义
static boost::json::object makeCleanCopy(const boost::json::object& src) {
    boost::json::object dst = src;   // 深度拷贝
    brutalEscapeJson(dst);
    return dst;
}


extern std::string publicKey;

extern std::string privateKey;


#endif // UTILS_H