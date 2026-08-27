
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "Utils.h"
#include "../signal/AsioConcurrentQueue.h"  // 你的队列头文件
#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/sam.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <future>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <share.h>
#endif

// ---------- 全局开关（声明在 Utils.h）----------
int consoleOutputLevels[4] = { 1, 1, 1, 1 };
int logToFileEnabled = 1;

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

// ---------- 异步队列条目（定长，零堆分配）----------
// 只携带调用线程能零成本拿到的信息：时间戳用整数微秒，文件名用 __FILE__ 指针。
// 字符串格式化（strftime / 文件行对齐 / 消息体）中，
// 消息体在调用线程用栈缓冲 vsnprintf 一次完成（短消息零分配）；
// 时间戳格式化与文件行对齐下沉到消费线程。
struct LogEntry {
    LogLevel level = LOG_LEVEL_INFO;
    int64_t timeMicros = 0;          // 微秒级 wall-clock，调用线程一次 system_clock::now
    const char* file = nullptr;     // __FILE__ 字面量指针，程序全程有效
    int line = 0;
    const char* levelStr = nullptr;   // 指向静态常量
    const char* color = nullptr;     // 指向静态常量
    char message[512] = {};          // 定长缓冲，短消息零堆分配
    int messageLen = 0;
    bool showConsole = true;
    bool writeFile = true;
};

// ---------- 异步基础设施 ----------
static std::unique_ptr<hope::signal::AsioConcurrentQueue<LogEntry>> asyncQueue = nullptr;
static boost::asio::io_context ioContext{ 1 };   // 单线程消费，文件操作无需加锁
static std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> logWork;
static std::thread ioThread;
static std::atomic<bool> stopped{ true };
static boost::asio::steady_timer* flushTimer = nullptr;

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

// 微秒级时间戳格式化（仅在消费线程调用，单线程安全）
// 自己拼数字，避免 strftime 的 locale 开销，且能输出微秒。
static void formatTimestampMicros(int64_t micros, char* buffer, size_t size) {
    time_t secs = static_cast<time_t>(micros / 1000000);
    int64_t frac = micros % 1000000;
    if (frac < 0) { // 容错：理论上 system_clock 不会为负
        secs -= 1;
        frac += 1000000;
    }
    struct tm timeinfo;
#ifdef _WIN32
    if (localtime_s(&timeinfo, &secs) != 0) {
        snprintf(buffer, size, "0000-00-00 00:00:00.000000");
        return;
    }
#else
    if (localtime_r(&secs, &timeinfo) == nullptr) {
        snprintf(buffer, size, "0000-00-00 00:00:00.000000");
        return;
    }
#endif
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%06lld",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
        static_cast<long long>(frac));
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
            // 全缓冲 + 定时 flush：减少 fwrite 系统调用次数
            setvbuf(logFiles[i], nullptr, _IOFBF, 64 * 1024);
        }
    }
}

static void closeLogFiles() {
    for (int i = 0; i < 4; i++) {
        if (logFiles[i]) {
            fflush(logFiles[i]);
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

// ---------- 日志处理协程（消费线程）----------
static boost::asio::awaitable<void> logProcessor() {
    // 初始化目录和文件（单线程，安全）
    ensureLogDirAndFiles();

    char ts[32];
    char fileLineBuf[160];
    char aligned[160];

    while (true) {

        LogEntry entry;
        if (!asyncQueue->tryDequeue(entry)
            && !co_await asyncQueue->awaitDequeue(entry)) {
            break;
        }

        // 时间戳格式化（下沉到消费线程）
        formatTimestampMicros(entry.timeMicros, ts, sizeof(ts));

        // 文件行截取 + 对齐（下沉到消费线程）
        const char* slash = strrchr(entry.file, '/');
        const char* backslash = strrchr(entry.file, '\\');
        const char* shortFile = (slash > backslash ? slash : backslash);
        shortFile = shortFile ? shortFile + 1 : entry.file;
        snprintf(fileLineBuf, sizeof(fileLineBuf), "%s:%d", shortFile, entry.line);
        snprintf(aligned, sizeof(aligned), "%-30s", fileLineBuf);

        // 控制台输出：fputs 拼接，避免 fprintf 的格式串解析
        if (entry.showConsole) {
            fputs(entry.color, stdout);
            fputc('[', stdout);
            fputs(ts, stdout);
            fputs("][", stdout);
            fputs(entry.levelStr, stdout);
            fputs("] ", stdout);
            fputs(aligned, stdout);
            fputc(' ', stdout);
            fwrite(entry.message, 1, static_cast<size_t>(entry.messageLen), stdout);
            fputs(COLOR_RESET, stdout);
            fputc('\n', stdout);
        }

        // 文件输出：同样用 fputs 拼接
        if (entry.writeFile && logToFileEnabled != 0) {
            FILE* f = logFiles[entry.level];
            if (f) {
                fputc('[', f);
                fputs(ts, f);
                fputs("][", f);
                fputs(entry.levelStr, f);
                fputs("] ", f);
                fputs(aligned, f);
                fputc(' ', f);
                fwrite(entry.message, 1, static_cast<size_t>(entry.messageLen), f);
                fputc('\n', f);
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

// ---------- 定时 flush 协程 ----------
static boost::asio::awaitable<void> flushTimerLoop() {
    try {
        while (!stopped.load(std::memory_order_relaxed)) {
            flushTimer->expires_after(std::chrono::milliseconds(100));
            boost::system::error_code ec;
            co_await flushTimer->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) {
                // 被 cancel（closeLogger 触发），退出
                break;
            }
            if (stopped.load(std::memory_order_relaxed)) break;
            for (int i = 0; i < 4; i++) {
                if (logFiles[i]) fflush(logFiles[i]);
            }
        }
    }
    catch (...) {
        // 吞掉异常，定时器协程不应影响主流程
    }
    co_return;
}

// ---------- 公开接口 ----------
void initLogger() {

    if (!stopped.exchange(false)) return;

    // 使用 work_guard 防止 io_context 空转退出
    logWork = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(ioContext));

    // 运行 io_context 的线程
    ioThread = std::thread([]() {

        ioContext.run();

        });

    std::packaged_task<void()> packagedTask([]() {});

    std::future<void> asyncQueueFuture = packagedTask.get_future();

    boost::asio::post(ioContext, [&packagedTask]() {

        // 使用 io_context 的执行器创建队列
        asyncQueue = std::make_unique<hope::signal::AsioConcurrentQueue<LogEntry>>(ioContext.get_executor());

        // 创建定时 flush 用的 timer
        flushTimer = new boost::asio::steady_timer(ioContext);

        // 启动日志处理协程
        boost::asio::co_spawn(ioContext, logProcessor, boost::asio::detached);

        // 启动定时 flush 协程
        boost::asio::co_spawn(ioContext, flushTimerLoop, boost::asio::detached);

        packagedTask();

        });

    asyncQueueFuture.get();

}

void closeLogger() {

    if (stopped.exchange(true)) return;

    if (!asyncQueue) return;

    // 通知队列关闭，协程将收到 nullopt 并退出
    asyncQueue->close();

    // 取消定时 flush，让 flushTimerLoop 立即退出
    if (flushTimer) {
        flushTimer->cancel();
    }

    // 撤销 work_guard，让 io_context 在协程自然结束后退出
    logWork.reset();

    // 等待 io_context 排空并结束
    if (ioThread.joinable()) {
        ioThread.join();
    }

    // 兜底
    ioContext.stop();

    delete flushTimer;
    flushTimer = nullptr;

    closeLogFiles();
    asyncQueue.reset();
}

void enableFileLogging(int enable) {
    logToFileEnabled = enable;
}

void setConsoleOutputLevels(int debug, int info, int warn, int error) {
    consoleOutputLevels[LOG_LEVEL_DEBUG] = debug;
    consoleOutputLevels[LOG_LEVEL_INFO] = info;
    consoleOutputLevels[LOG_LEVEL_WARN] = warn;
    consoleOutputLevels[LOG_LEVEL_ERROR] = error;
}

void setLogDirectory(const char* dir) {
    if (stopped.load(std::memory_order_relaxed)) return;
    if (!asyncQueue) return;

    std::string newDir = dir ? dir : "";
    boost::asio::post(ioContext, [newDir = std::move(newDir)]() {
        switchLogDirectory(newDir);
        });
}

static void enqueueLog(LogLevel level, const char* file, int line,
    const char* format, va_list args,
    bool showConsole, bool writeFile) {
    if (!asyncQueue || stopped.load(std::memory_order_relaxed)) return;
    if (!showConsole && !writeFile) return;

    LogEntry entry;
    entry.level = level;

    entry.timeMicros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    entry.file = file;      // __FILE__ 字面量，零成本
    entry.line = line;
    getLevelInfo(level, &entry.levelStr, &entry.color);
    entry.showConsole = showConsole;
    entry.writeFile = writeFile;

    // 栈缓冲一次 vsnprintf：短消息零堆分配
    char stackbuf[sizeof(LogEntry::message)];
    int len = vsnprintf(stackbuf, sizeof(stackbuf), format, args);
    if (len < 0) {
        entry.messageLen = 0;
    }
    else if (len < (int)sizeof(stackbuf)) {
        memcpy(entry.message, stackbuf, static_cast<size_t>(len));
        entry.messageLen = len;
    }
    else {
        // 超长截断
        memcpy(entry.message, stackbuf, sizeof(entry.message) - 1);
        entry.messageLen = static_cast<int>(sizeof(entry.message) - 1);
    }

    asyncQueue->enqueue(std::move(entry));
}

void logMessage(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args,
        consoleOutputLevels[level] != 0,
        logToFileEnabled != 0);
    va_end(args);
}

void logMessagePlain(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args,
        consoleOutputLevels[level] != 0,
        logToFileEnabled != 0);
    va_end(args);
}

void logToFileOnly(LogLevel level, const char* file, int line, const char* format, ...) {
    va_list args;
    va_start(args, format);
    enqueueLog(level, file, line, format, args, false, true);
    va_end(args);
}