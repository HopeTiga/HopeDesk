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

#ifdef _MSC_VER
#include <intrin.h>  // MSVC 需要这个
#pragma intrinsic(_mm256_fmadd_ps)
#else
#include <cpuid.h>   // GCC/Clang 需要这个
#endif

// 日志级别枚举
typedef enum {
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_DEBUG
} LogLevel;

// 日志的宏定义
#define LOG_INFO(fmt, ...)    log_message(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) log_message(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   log_message(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   log_message(LOG_DEBUG, fmt, ##__VA_ARGS__)

// 无颜色版本的宏
#define LOG_INFO_PLAIN(fmt, ...)    log_message_plain(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARNING_PLAIN(fmt, ...) log_message_plain(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR_PLAIN(fmt, ...)   log_message_plain(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_PLAIN(fmt, ...)   log_message_plain(LOG_DEBUG, fmt, ##__VA_ARGS__)

// ANSI颜色代码
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[91m"
#define COLOR_GREEN   "\033[92m"
#define COLOR_YELLOW  "\033[93m"
#define COLOR_BLUE    "\033[94m"

// 函数声明
void get_timestamp(char* buffer, size_t size);
void get_level_info(LogLevel level, const char** level_str, const char** color);
void log_message(LogLevel level, const char* format, ...);
void log_message_plain(LogLevel level, const char* format, ...);

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

    // 小数据直接用 memcpy（编译器优化更好）
    if (size < 128) {
        memcpy(d, s, size);
        return;
    }

    // 检查 AVX2 支持
    if (!hasAVX2()) {
        memcpy(d, s, size);
        return;
    }

    // AVX2 优化路径
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

    // 处理剩余
    size_t remaining = size % 128;
    if (remaining > 0) {
        memcpy(d, s, remaining);
    }
}

#endif // UTILS_H