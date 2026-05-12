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

#ifdef __cplusplus
extern "C" {
#endif

    typedef enum {
        LOG_LEVEL_DEBUG,
        LOG_LEVEL_INFO,
        LOG_LEVEL_WARN,
        LOG_LEVEL_ERROR
    } LogLevel;

    void initLogger();
    void closeLogger();
    void enableFileLogging(int enable);
    void setLogDirectory(const char* dir);
    void setConsoleOutputLevels(int debug, int info, int warn, int error);

    void logMessage(LogLevel level, const char* file, int line, const char* format, ...);
    void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...);
    void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...);

    void getTimestamp(char* buffer, size_t size);
    void getLevelInfo(LogLevel level, const char** levelStr, const char** color);

#define LOG_INFO(fmt, ...)    logMessage(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    logMessage(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__) 
#define LOG_ERROR(fmt, ...)   logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   logMessage(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO_PLAIN(fmt, ...)    logMessagePlain(LOG_LEVEL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN_PLAIN(fmt, ...)    logMessagePlain(LOG_LEVEL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR_PLAIN(fmt, ...)   logMessagePlain(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_PLAIN(fmt, ...)   logMessagePlain(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // UTILS_H