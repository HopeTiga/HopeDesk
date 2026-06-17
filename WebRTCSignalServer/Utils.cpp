#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Utils.h"
#include "AsioConcurrentQueue.h"  // 你的队列头文件
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/sam.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <share.h>
#endif

// ---------- 颜色常量 ----------
static const char* COLOR_RESET = "\033[0m";
static const char* COLOR_RED = "\033[91m";
static const char* COLOR_GREEN = "\033[92m";
static const char* COLOR_YELLOW = "\033[93m";
static const char* COLOR_BLUE = "\033[94m";

// ---------- 文件与目录 ----------
static const char* logFileNames[4] = {
    "debug.log", "info.log", "warn.log", "error.log"
};
static std::string logDir = "logs";
static FILE* logFiles[4] = { nullptr, nullptr, nullptr, nullptr };

// ---------- 原子控制开关 ----------
static std::atomic<int> logToFileEnabled{ 1 };
static std::atomic<int> consoleOutputLevels[4] = { {1}, {1}, {1}, {1} };

// ---------- 异步队列条目（唯一的数据载体）----------
struct LogEntry {
    enum Type { LOG, CMD_SET_DIR };
    Type type = LOG;

    // LOG 专用字段
    LogLevel level = LOG_LEVEL_INFO;
    char timestamp[32] = {};
    const char* levelStr = nullptr;  // 指向静态常量
    const char* color = nullptr;     // 指向静态常量
    std::string message;             // 已包含文件行号与内容
    bool showConsole = true;
    bool writeFile = true;

    // CMD_SET_DIR 专用字段
    std::string newDir;
};

// ---------- 异步基础设施 ----------
static std::unique_ptr<hope::core::AsioConcurrentQueue<LogEntry>> asyncQueue;
static boost::asio::io_context ioContext;   // 可以多个线程，但此处仅用 1 个
static std::thread ioThread;
static std::atomic<bool> stopped{ false };

// ---------- 工具函数 ----------
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
    const char* shortFile = (slash > backslash ? slash : backslash);
    shortFile = shortFile ? shortFile + 1 : file;
    char buffer[128], aligned[128];
    snprintf(buffer, sizeof(buffer), "%s:%d", shortFile, line);
    snprintf(aligned, sizeof(aligned), "%-30s", buffer);
    return aligned;
}

static std::string formatString(const char* format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);
    if (len <= 0) return "";
    std::vector<char> buf(len + 1);
    vsnprintf(buf.data(), len + 1, format, args);
    return std::string(buf.data(), len);
}

// ---------- 文件操作（仅在 io_context 线程中调用）----------
static void ensureLogDirAndFiles() {
#ifdef _WIN32
    if (!CreateDirectoryA(logDir.c_str(), NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            fprintf(stderr, "ERROR: Failed to create log dir: %s (Error %lu)\n", logDir.c_str(), err);
    }
#else
    mkdir(logDir.c_str(), 0755);
#endif
    for (int i = 0; i < 4; i++) {
        if (logFiles[i]) continue;
        std::string path = logDir + "/" + logFileNames[i];
#ifdef _WIN32
        logFiles[i] = _fsopen(path.c_str(), "a", _SH_DENYNO);
#else
        logFiles[i] = fopen(path.c_str(), "a");
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

static void switchLogDirectory(const std::string& newDir) {
    closeLogFiles();
    logDir = newDir;
    ensureLogDirAndFiles();
}

// ---------- 日志处理协程 ----------
static boost::asio::awaitable<void> logProcessor() {
    // 初始化目录和文件（单线程，安全）
    ensureLogDirAndFiles();

    while (true) {
        auto optEntry = co_await asyncQueue->dequeue();
        if (!optEntry.has_value()) {
            // 队列已关闭
            break;
        }

        LogEntry& entry = *optEntry;

        if (entry.type == LogEntry::CMD_SET_DIR) {
            switchLogDirectory(entry.newDir);
            continue;
        }

        // 普通日志条目
        if (entry.showConsole) {
            printf("%s[%s][%-5s] %s%s\n",
                entry.color,
                entry.timestamp,
                entry.levelStr,
                entry.message.c_str(),
                COLOR_RESET);
        }

        if (entry.writeFile && logToFileEnabled.load(std::memory_order_relaxed)) {
            FILE* f = logFiles[entry.level];
            if (f) {
                fprintf(f, "[%s][%-5s] %s\n",
                    entry.timestamp,
                    entry.levelStr,
                    entry.message.c_str());
            }
            else {
                fprintf(stderr, "Logger Error: Cannot write to %s\n", logFileNames[entry.level]);
            }
        }
    }

    // 清理
    closeLogFiles();
    co_return;
}

// ---------- 公开接口 ----------
void initLogger() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        // 使用 io_context 的执行器创建队列
        asyncQueue = std::make_unique<hope::core::AsioConcurrentQueue<LogEntry>>(
            ioContext.get_executor());

        // 启动日志处理协程
        boost::asio::co_spawn(ioContext, logProcessor, boost::asio::detached);

        // 运行 io_context 的线程
        ioThread = std::thread([]() {
            ioContext.run();
            });
        });
}

void closeLogger() {
    if (!asyncQueue) return;

    // 通知队列关闭，协程将收到 nullopt 并退出
    asyncQueue->close();

    // 等待 io_context 完成所有任务并退出
    ioContext.stop();
    if (ioThread.joinable()) {
        ioThread.join();
    }

    // 兜底关闭文件（协程退出时已做过，这里以防万一）
    closeLogFiles();
    asyncQueue.reset();
}

void enableFileLogging(int enable) {
    logToFileEnabled.store(enable ? 1 : 0, std::memory_order_relaxed);
}

void setConsoleOutputLevels(int debug, int info, int warn, int error) {
    consoleOutputLevels[LOG_LEVEL_DEBUG].store(debug, std::memory_order_relaxed);
    consoleOutputLevels[LOG_LEVEL_INFO].store(info, std::memory_order_relaxed);
    consoleOutputLevels[LOG_LEVEL_WARN].store(warn, std::memory_order_relaxed);
    consoleOutputLevels[LOG_LEVEL_ERROR].store(error, std::memory_order_relaxed);
}

// 目录切换通过专用命令入队，保证与日志写入的顺序性
void setLogDirectory(const char* dir) {
    if (!asyncQueue) return;

    LogEntry cmd;
    cmd.type = LogEntry::CMD_SET_DIR;
    cmd.newDir = dir;
    asyncQueue->enqueue(std::move(cmd));
}

// ---------- 日志写入入口（仅构造并入队，极快）----------
static void enqueueLog(LogLevel level, const char* file, int line,
    const char* format, va_list args,
    bool showConsole, bool writeFile) {
    if (!asyncQueue) return;
    if (!showConsole && !writeFile) return;

    LogEntry entry;
    entry.type = LogEntry::LOG;
    entry.level = level;
    getTimestamp(entry.timestamp, sizeof(entry.timestamp));
    getLevelInfo(level, &entry.levelStr, &entry.color);

    std::string msg = formatString(format, args);
    std::string fileLine = formatFileAndLine(file, line);
    entry.message = fileLine + " " + msg;
    entry.showConsole = showConsole;
    entry.writeFile = writeFile;

    asyncQueue->enqueue(std::move(entry));
}

void logMessage(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args,
        consoleOutputLevels[level].load(std::memory_order_relaxed) != 0,
        logToFileEnabled.load(std::memory_order_relaxed) != 0);
    va_end(args);
}

void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args,
        consoleOutputLevels[level].load(std::memory_order_relaxed) != 0,
        logToFileEnabled.load(std::memory_order_relaxed) != 0);
    va_end(args);
}

void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args, false, true);
    va_end(args);
}