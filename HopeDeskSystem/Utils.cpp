#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Utils.h"
#include <chrono>
#include <vector>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
static int loggerInitialized = 0;
static int consoleOutputLevels[4] = { 1, 1, 1, 1 };

#ifdef _WIN32
static HANDLE logHandles[4] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
#else
static FILE* logFiles[4] = { nullptr, nullptr, nullptr, nullptr };
#endif

static void openLogFiles() {
    for (int i = 0; i < 4; i++) {
#ifdef _WIN32
        if (logHandles[i] != INVALID_HANDLE_VALUE) continue;
        std::string filePath = logDir + "\\" + logFileNames[i];
        HANDLE h = CreateFileA(
            filePath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (h != INVALID_HANDLE_VALUE) {
            logHandles[i] = h;
        }
#else
        if (logFiles[i]) continue;
        std::string filePath = logDir + "/" + logFileNames[i];
        logFiles[i] = fopen(filePath.c_str(), "a");
#endif
    }
}

static void closeLogFiles() {
    for (int i = 0; i < 4; i++) {
#ifdef _WIN32
        if (logHandles[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(logHandles[i]);
            logHandles[i] = INVALID_HANDLE_VALUE;
        }
#else
        if (logFiles[i]) {
            fclose(logFiles[i]);
            logFiles[i] = nullptr;
        }
#endif
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
    ensureLogDirectory();
}

void closeLogger() {
    closeLogFiles();
    loggerInitialized = 0;
}

void enableFileLogging(int enable) {
    logToFileEnabled = enable;
}

void setLogDirectory(const char* dir) {
    closeLogFiles();
    logDir = dir;
    loggerInitialized = 0;
    ensureLogDirectory();
}

void setConsoleOutputLevels(int debug, int info, int warn, int error) {
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
    case LOG_LEVEL_WARN:  *levelStr = "WARN";  *color = COLOR_YELLOW; break;
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

static void writeConsole(const char* data, size_t len) {
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, (DWORD)len, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, nullptr);
#else
    fwrite(data, 1, len, stdout);
    fwrite("\n", 1, 1, stdout);
    fflush(stdout);
#endif
}

static void writeFileRaw(HANDLE hFile, const char* data, size_t len) {
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, data, (DWORD)len, &written, nullptr);
    WriteFile(hFile, "\r\n", 2, &written, nullptr);
    FlushFileBuffers(hFile);
}

static void doLog(LogLevel level, const char* file, int line, const char* format, va_list args, bool plain, bool fileOnly) {
    char timestamp[32];
    const char* levelStr;
    const char* color;

    getTimestamp(timestamp, sizeof(timestamp));
    getLevelInfo(level, &levelStr, &color);

    std::string msg = formatString(format, args);
    std::string alignedFileLine = formatFileAndLine(file, line);

    int levelIdx = static_cast<int>(level);

    if (!fileOnly && levelIdx >= 0 && levelIdx <= 3 && consoleOutputLevels[levelIdx]) {
        char consoleBuf[4096];
        int n = 0;
        if (plain) {
            n = snprintf(consoleBuf, sizeof(consoleBuf), "[%s][%-5s] %s %s",
                timestamp, levelStr, alignedFileLine.c_str(), msg.c_str());
        }
        else {
            n = snprintf(consoleBuf, sizeof(consoleBuf), "%s[%s][%-5s] %s %s%s",
                color, timestamp, levelStr, alignedFileLine.c_str(), msg.c_str(), COLOR_RESET);
        }
        if (n > 0 && n < (int)sizeof(consoleBuf)) {
            writeConsole(consoleBuf, n);
        }
    }

    if (logToFileEnabled && levelIdx >= 0 && levelIdx <= 3) {
        ensureLogDirectory();

        char fileBuf[4096];
        int n = snprintf(fileBuf, sizeof(fileBuf), "[%s][%-5s] %s %s",
            timestamp, levelStr, alignedFileLine.c_str(), msg.c_str());

        if (n > 0 && n < (int)sizeof(fileBuf)) {
#ifdef _WIN32
            writeFileRaw(logHandles[levelIdx], fileBuf, n);
#else
            if (logFiles[levelIdx]) {
                fwrite(fileBuf, 1, n, logFiles[levelIdx]);
                fwrite("\n", 1, 1, logFiles[levelIdx]);
                fflush(logFiles[levelIdx]);
            }
#endif
        }
    }
}

void logMessage(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    doLog(level, file, line, format, args, false, false);
    va_end(args);
}

void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    doLog(level, file, line, format, args, true, false);
    va_end(args);
}

void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    doLog(level, file, line, format, args, false, true);
    va_end(args);
}