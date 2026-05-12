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
#ifdef _WIN32
#include <direct.h>
#define mkdir(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _MSC_VER
#include <intrin.h> 
#pragma intrinsic(_mm256_fmadd_ps)
#else
#include <cpuid.h> 
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