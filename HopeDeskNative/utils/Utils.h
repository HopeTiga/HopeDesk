#pragma once
#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <immintrin.h>
#include <cstring>
#include <cstdint>
#include <string>
#include <mutex>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdirs(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <unistd.h>
typedef void* HCURSOR;
#endif

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_mm256_fmadd_ps)
#else
#include <cpuid.h>
#endif

// ---- spdlog（header-only，内置 fmt）----
// 完整 spdlog 只在 Utils.cpp 编译；其余 TU 只用到 fmt，供 LOG_* 宏做编译期格式校验。
#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif
#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>

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
void initWebrtcLogging();    // 安装 webrtc RTC_LOG sink,写 logs/webrtc.log(排查 ICE/DTLS)
void closeWebrtcLogging();
void enableFileLogging(int enable);
void setLogDirectory(const char* dir);
void setConsoleOutputLevels(int debug, int info, int warn, int error);

// 级别开关（供宏在调用点短路：被关掉的级别连格式化都不做）
extern int consoleOutputLevels[4];
extern int logToFileEnabled;

void getTimestamp(char* buffer, size_t size);
void getLevelInfo(LogLevel level, const char** levelStr, const char** color);

#ifdef __cplusplus
}
#endif

namespace hope::log {

// 已格式化消息入口，实现于 Utils.cpp（只有该 TU 编译完整 spdlog）
void logMessage(LogLevel level, const char* file, int line, const std::string& message, bool fileOnly);

// fmt 入口：在调用线程完成编译期校验与格式化
template <typename... Args>
inline void logMessage(LogLevel level, const char* file, int line, fmt::format_string<Args...> format, Args&&... args) {
    logMessage(level, file, line, fmt::format(format, std::forward<Args>(args)...), false);
}

// 只写文件、不上控制台
template <typename... Args>
inline void logToFileOnly(LogLevel level, const char* file, int line, fmt::format_string<Args...> format, Args&&... args) {
    logMessage(level, file, line, fmt::format(format, std::forward<Args>(args)...), true);
}

} // namespace hope::log

// 便捷宏（fmt 风格 {} 占位符；级别过滤前置，被关掉的级别连格式化都不做）
#define LOG_DEBUG(...) do { if (consoleOutputLevels[LOG_LEVEL_DEBUG] != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__); } while(0)
#define LOG_INFO(...)  do { if (consoleOutputLevels[LOG_LEVEL_INFO]  != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__); } while(0)
#define LOG_WARN(...)  do { if (consoleOutputLevels[LOG_LEVEL_WARN]  != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__); } while(0)
#define LOG_ERROR(...) do { if (consoleOutputLevels[LOG_LEVEL_ERROR] != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__); } while(0)
// 位置由调用方给出：要报的不一定是这一行（如 CompletionHandle 报的是令牌的构造点）
#define LOG_ERROR_FROM(sourceLocation, ...) do { if (consoleOutputLevels[LOG_LEVEL_ERROR] != 0 || logToFileEnabled != 0) hope::log::logMessage(LOG_LEVEL_ERROR, (sourceLocation).file_name(), static_cast<int>((sourceLocation).line()), __VA_ARGS__); } while(0)

#define LOG_DEBUG_PLAIN(...) LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO_PLAIN(...)  LOG_INFO(__VA_ARGS__)
#define LOG_WARN_PLAIN(...)  LOG_WARN(__VA_ARGS__)
#define LOG_ERROR_PLAIN(...) LOG_ERROR(__VA_ARGS__)

HCURSOR CreateCursorFromRGBA(unsigned char* rgbaData, int width, int height, int hotX = 0, int hotY = 0);

inline bool hasAVX2() {
    static bool checked = false;
    static bool supported = false;

    if (!checked) {
#ifdef _MSC_VER
        int cpuinfo[4];
        __cpuid(cpuinfo, 7);
        supported = (cpuinfo[1] & (1 << 5)) != 0;
#else
        unsigned int eax, ebx, ecx, edx;
        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        supported = (ebx & (1 << 5)) != 0;
#endif
        checked = true;
    }
    return supported;
}

inline void fastCopy(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (size < 128) {
        memcpy(d, s, size);
        return;
    }

    if (!hasAVX2()) {
        memcpy(d, s, size);
        return;
    }

    size_t chunks = size / 128;
    for (size_t i = 0; i < chunks; i++) {
        __m256i v0 = _mm256_loadu_si256((const __m256i*)(s + 0));
        __m256i v1 = _mm256_loadu_si256((const __m256i*)(s + 32));
        __m256i v2 = _mm256_loadu_si256((const __m256i*)(s + 64));
        __m256i v3 = _mm256_loadu_si256((const __m256i*)(s + 96));

        _mm256_storeu_si256((__m256i*)(d + 0), v0);
        _mm256_storeu_si256((__m256i*)(d + 32), v1);
        _mm256_storeu_si256((__m256i*)(d + 64), v2);
        _mm256_storeu_si256((__m256i*)(d + 96), v3);

        s += 128;
        d += 128;
    }

    size_t remaining = size % 128;
    if (remaining > 0) {
        memcpy(d, s, remaining);
    }
}

#endif // UTILS_H
