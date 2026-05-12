#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Utils.h"
#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <share.h>
#endif

static const char* COLOR_RESET = "\033[0m";
static const char* COLOR_RED = "\033[91m";
static const char* COLOR_GREEN = "\033[92m";
static const char* COLOR_YELLOW = "\033[93m";
static const char* COLOR_BLUE = "\033[94m";

static const char* logFileNames[4] = {
    "debug.log",
    "info.log",
    "warn.log",
    "error.log"
};
static std::string logDir = "logs";
static int logToFileEnabled = 1;
static std::mutex logMutex;
static int loggerInitialized = 0;

static FILE* logFiles[4] = { nullptr, nullptr, nullptr, nullptr };

static int consoleOutputLevels[4] = { 1, 1, 1, 1 };

static void openLogFiles() {
    for (int i = 0; i < 4; i++) {
        if (logFiles[i]) continue;
        std::string filePath = logDir + "/" + logFileNames[i];
#ifdef _WIN32
        logFiles[i] = _fsopen(filePath.c_str(), "a", _SH_DENYNO);
#else
        logFiles[i] = fopen(filePath.c_str(), "a");
#endif
        if (logFiles[i]) {
#ifdef _WIN32
            setvbuf(logFiles[i], nullptr, _IOLBF, 4096);
#else
            setvbuf(logFiles[i], nullptr, _IOLBF, 0);
#endif
        }
    }
}

static void closeLogFiles() {
    for (int i = 0; i < 4; i++) {
        if (logFiles[i]) {
            fclose(logFiles[i]);
            logFiles[i] = nullptr;
        }
    }
}

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
    openLogFiles();
    loggerInitialized = 1;
}

void initLogger() {
    std::lock_guard<std::mutex> lock(logMutex);
    ensureLogDirectory();
}

void closeLogger() {
    std::lock_guard<std::mutex> lock(logMutex);
    closeLogFiles();
    loggerInitialized = 0;
}

void enableFileLogging(int enable) {
    std::lock_guard<std::mutex> lock(logMutex);
    logToFileEnabled = enable;
}

void setLogDirectory(const char* dir) {
    std::lock_guard<std::mutex> lock(logMutex);
    closeLogFiles();
    logDir = dir;
    loggerInitialized = 0;
}

void setConsoleOutputLevels(int debug, int info, int warn, int error) {
    std::lock_guard<std::mutex> lock(logMutex);
    consoleOutputLevels[LOG_LEVEL_DEBUG] = debug;
    consoleOutputLevels[LOG_LEVEL_INFO] = info;
    consoleOutputLevels[LOG_LEVEL_WARN] = warn;
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
    case LOG_LEVEL_INFO:  *levelStr = "INFO";  *color = COLOR_GREEN; break;
    case LOG_LEVEL_WARN:  *levelStr = "WARN";  *color = COLOR_YELLOW; break; // 返回 WARN
    case LOG_LEVEL_ERROR: *levelStr = "ERROR"; *color = COLOR_RED; break;
    case LOG_LEVEL_DEBUG: *levelStr = "DEBUG"; *color = COLOR_BLUE; break;
    default:              *levelStr = "UNKN";  *color = COLOR_RESET; break;
    }
}

static std::string formatFileAndLine(const char* file, int line) {

    const char* slash = strrchr(file, '/');
    const char* backslash = strrchr(file, '\\');
    const char* shortFile = slash > backslash ? slash : backslash;
    shortFile = shortFile ? shortFile + 1 : file;

    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%s:%d", shortFile, line);

    char aligned[128];

    snprintf(aligned, sizeof(aligned), "%-30s", buffer);
    return std::string(aligned);
}

static void writeRawToFile(LogLevel level, const char* timestamp, const char* levelStr, const std::string& message) {
    if (!logToFileEnabled || level < 0 || level > 3) return;

    ensureLogDirectory();
    FILE* logFile = logFiles[level];

    if (logFile) {
        fprintf(logFile, "[%s][%-5s] %s\n", timestamp, levelStr, message.c_str());
    }
    else {
        fprintf(stderr, "Logger Error: Cannot write to %s\n", logFileNames[level]);
    }
}

static std::string formatString(const char* format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);

    int len = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    if (len <= 0) return "";

    std::vector<char> buffer(len + 1);
    vsnprintf(buffer.data(), len + 1, format, args);

    return std::string(buffer.data(), len);
}

void logMessage(LogLevel level, const char* file, int line, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    std::string alignedFileLine = formatFileAndLine(file, line);
    std::string fullMsg = alignedFileLine + " " + msg;

    std::lock_guard<std::mutex> lock(logMutex);

    if (consoleOutputLevels[level]) {
        printf("%s[%s][%-5s] %s%s\n", color, timestamp, levelStr, fullMsg.c_str(), COLOR_RESET);
    }

    writeRawToFile(level, timestamp, levelStr, fullMsg);
}

void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    std::string alignedFileLine = formatFileAndLine(file, line);
    std::string fullMsg = alignedFileLine + " " + msg;

    std::lock_guard<std::mutex> lock(logMutex);

    if (consoleOutputLevels[level]) {
        printf("[%s][%-5s] %s\n", timestamp, levelStr, fullMsg.c_str());
    }

    writeRawToFile(level, timestamp, levelStr, fullMsg);
}

void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    va_list args;
    va_start(args, format);
    std::string msg = formatString(format, args);
    va_end(args);

    std::string alignedFileLine = formatFileAndLine(file, line);
    std::string fullMsg = alignedFileLine + " " + msg;

    std::lock_guard<std::mutex> lock(logMutex);
    writeRawToFile(level, timestamp, levelStr, fullMsg);
}

HCURSOR CreateCursorFromRGBA(unsigned char* rgbaData, int width, int height, int hotX, int hotY)
{
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    ReleaseDC(NULL, hdc);
    if (!hdcMem) return NULL;

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

    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    if (!hMask) {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        return NULL;
    }

    int maskRowBytes = ((width + 15) / 16) * 2;
    BYTE* maskBits = new BYTE[maskRowBytes * height];
    memset(maskBits, 0xFF, maskRowBytes * height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            BYTE alpha = rgbaData[idx + 3];

            if (alpha > 128) {
                int byteIdx = y * maskRowBytes + (x / 8);
                int bitPos = 7 - (x % 8);
                maskBits[byteIdx] &= ~(1 << bitPos);
            }
        }
    }

    SetBitmapBits(hMask, maskRowBytes * height, maskBits);
    delete[] maskBits;

    ICONINFO iconInfo = {};
    iconInfo.fIcon = FALSE;
    iconInfo.xHotspot = hotX;
    iconInfo.yHotspot = hotY;
    iconInfo.hbmMask = hMask;
    iconInfo.hbmColor = hBitmap;

    HCURSOR cursor = CreateIconIndirect(&iconInfo);

    DeleteObject(hBitmap);
    DeleteObject(hMask);
    DeleteDC(hdcMem);

    return cursor;
}
