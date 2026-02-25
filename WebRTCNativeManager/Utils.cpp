#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Utils.h"
#include <chrono>
#include <mutex>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <share.h> // 必须包含这个以使用 _fsopen 和 _SH_DENYNO
#endif

// 颜色定义
static const char* COLOR_RESET = "\033[0m";
static const char* COLOR_RED = "\033[91m";
static const char* COLOR_GREEN = "\033[92m";
static const char* COLOR_YELLOW = "\033[93m";
static const char* COLOR_BLUE = "\033[94m";

// 全局变量
static const char* logFileNames[4] = {
    "debug.log",
    "info.log",
    "warning.log",
    "error.log"
};
static std::string logDir = "logs";
static int logToFileEnabled = 1;
static std::mutex logMutex;
static int loggerInitialized = 0;

// 哪些级别需要输出到控制台
static int consoleOutputLevels[4] = { 1, 1, 1, 1 };

// 内部辅助：确保目录存在
static void ensureLogDirectory() {
    if (loggerInitialized) return;

#ifdef _WIN32
    if (!CreateDirectoryA(logDir.c_str(), NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            fprintf(stderr, "ERROR: Failed to create log dir: %s (Error %lu)\n", logDir.c_str(), err);
        }
    }
#else
    mkdir(logDir.c_str(), 0755);
#endif
    loggerInitialized = 1;
}

void initLogger() {
    std::lock_guard<std::mutex> lock(logMutex);
    ensureLogDirectory();
}

void closeLogger() {
    std::lock_guard<std::mutex> lock(logMutex);
    loggerInitialized = 0;
}

void enableFileLogging(int enable) {
    std::lock_guard<std::mutex> lock(logMutex);
    logToFileEnabled = enable;
}

void setLogDirectory(const char* dir) {
    std::lock_guard<std::mutex> lock(logMutex);
    logDir = dir;
    loggerInitialized = 0; // 重置以重新创建目录
}

void setConsoleOutputLevels(int debug, int info, int warning, int error) {
    std::lock_guard<std::mutex> lock(logMutex);
    consoleOutputLevels[LOG_LEVEL_DEBUG] = debug;
    consoleOutputLevels[LOG_LEVEL_INFO] = info;
    consoleOutputLevels[LOG_LEVEL_WARNING] = warning;
    consoleOutputLevels[LOG_LEVEL_ERROR] = error;
}

void getTimestamp(char* buffer, size_t size) {
    time_t rawtime;
    struct tm timeinfo;
    time(&rawtime);

#ifdef _WIN32
    if (localtime_s(&timeinfo, &rawtime) != 0) {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return;
    }
#else
    if (localtime_r(&rawtime, &timeinfo) == nullptr) {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return;
    }
#endif
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &timeinfo);
}

void getLevelInfo(LogLevel level, const char** levelStr, const char** color) {
    switch (level) {
    case LOG_LEVEL_INFO:    *levelStr = "INFO";    *color = COLOR_GREEN; break;
    case LOG_LEVEL_WARNING: *levelStr = "WARNING"; *color = COLOR_YELLOW; break;
    case LOG_LEVEL_ERROR:   *levelStr = "ERROR";   *color = COLOR_RED; break;
    case LOG_LEVEL_DEBUG:   *levelStr = "DEBUG";   *color = COLOR_BLUE; break;
    default:                *levelStr = "UNKNOWN"; *color = COLOR_RESET; break;
    }
}

// 核心：打开文件（支持共享读取）
static FILE* openLogFileShared(const std::string& path) {
    FILE* fp = nullptr;

#ifdef _WIN32
    // 使用 _fsopen 并指定 _SH_DENYNO (允许读写共享)
    // 这样你在用 VSCode/记事本看日志时，程序不会 crash 或写入失败
    fp = _fsopen(path.c_str(), "a", _SH_DENYNO);
#else
    // Linux/Mac 默认通常允许共享读
    fp = fopen(path.c_str(), "a");
#endif
    return fp;
}

// 核心：将已格式化的内容写入文件
static void writeRawToFile(LogLevel level, const char* timestamp, const char* levelStr, const std::string& message) {
    if (!logToFileEnabled || level < 0 || level > 3) return;

    ensureLogDirectory();
    std::string filePath = logDir + "/" + logFileNames[level];

    // 尝试打开文件
    FILE* logFile = openLogFileShared(filePath);

    // 如果打开失败（极少数情况），尝试重试一次
    if (!logFile) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        logFile = openLogFileShared(filePath);
    }

    if (logFile) {
        // [时间][级别] 消息
        fprintf(logFile, "[%s][%s] %s\n", timestamp, levelStr, message.c_str());

        // 关键：强制刷新缓冲区，确保外部编辑器能立即看到最新内容
        fflush(logFile);

        fclose(logFile);
    }
    else {
        // 如果还是失败，输出到 stderr 以便调试
        fprintf(stderr, "Logger Error: Cannot open file %s\n", filePath.c_str());
    }
}

// 辅助：格式化字符串的通用函数
static std::string formatString(const char* format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);

    // 第一次尝试：获取所需长度
    int len = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (len <= 0) return "";

    // 动态分配缓冲区 (防止 4096 截断问题)
    std::vector<char> buffer(len + 1);
    vsnprintf(buffer.data(), len + 1, format, args);

    return std::string(buffer.data(), len);
}

void logMessage(LogLevel level, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    // 锁保护：Console 输出和文件写入
    std::lock_guard<std::mutex> lock(logMutex);

    // 1. 输出到控制台
    if (consoleOutputLevels[level]) {
        printf("%s[%s][%s] %s%s\n", color, levelStr, timestamp, msg.c_str(), COLOR_RESET);
        // fflush(stdout); // 可选，视性能需求而定
    }

    // 2. 输出到文件
    writeRawToFile(level, timestamp, levelStr, msg);
}

void logMessagePlain(LogLevel level, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(logMutex);

    if (consoleOutputLevels[level]) {
        printf("[%s][%s] %s\n", levelStr, timestamp, msg.c_str());
    }

    writeRawToFile(level, timestamp, levelStr, msg);
}

void logToFileOnly(LogLevel level, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(logMutex);
    writeRawToFile(level, timestamp, levelStr, msg);
}


HCURSOR CreateCursorFromRGBA(unsigned char* rgbaData, int width, int height, int hotX, int hotY)
{
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    ReleaseDC(NULL, hdc);
    if (!hdcMem) return NULL;

    // 1. 准备32位颜色位图
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) {
        DeleteDC(hdcMem);
        return NULL;
    }

    // 需要将RGB转换为BGR，因为Windows DIB期望BGR格式
    // 但是如果getCursorBitmapData输出的是RGB，我们需要转换
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        BYTE r = rgbaData[idx];     // R
        BYTE g = rgbaData[idx + 1]; // G
        BYTE b = rgbaData[idx + 2]; // B
        BYTE a = rgbaData[idx + 3]; // A

        // 转换为BGR格式写入DIB
        ((BYTE*)pBits)[idx] = r;     // B
        ((BYTE*)pBits)[idx + 1] = g; // G
        ((BYTE*)pBits)[idx + 2] = b; // R
        ((BYTE*)pBits)[idx + 3] = a; // A
    }

    // 2. 创建单色掩码位图（1位）
    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    if (!hMask) {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        return NULL;
    }

    // 3. 根据Alpha通道生成掩码
    // 掩码规则：0（黑色）= 不透明，1（白色）= 透明
    int maskRowBytes = ((width + 15) / 16) * 2;
    BYTE* maskBits = new BYTE[maskRowBytes * height];
    memset(maskBits, 0xFF, maskRowBytes * height); // 初始化为全白（全透明）

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            BYTE alpha = rgbaData[idx + 3];

            // Alpha > 128 的像素设为黑色（不透明）
            if (alpha > 128) {
                int byteIdx = y * maskRowBytes + (x / 8);
                int bitPos = 7 - (x % 8);
                maskBits[byteIdx] &= ~(1 << bitPos); // 清除位（设为0/黑色）
            }
        }
    }

    SetBitmapBits(hMask, maskRowBytes * height, maskBits);
    delete[] maskBits;

    // 4. 创建光标
    ICONINFO iconInfo = {};
    iconInfo.fIcon = FALSE; // 创建光标
    iconInfo.xHotspot = hotX;
    iconInfo.yHotspot = hotY;
    iconInfo.hbmMask = hMask;    // 单色掩码
    iconInfo.hbmColor = hBitmap; // 颜色位图

    HCURSOR cursor = CreateIconIndirect(&iconInfo);

    // 5. 清理资源
    DeleteObject(hBitmap);
    DeleteObject(hMask);
    DeleteDC(hdcMem);

    return cursor;
}
