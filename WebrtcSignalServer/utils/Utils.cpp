
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

// 仅本 TU 编译完整 spdlog（header-only），其余 TU 只看到内置 fmt
// SPDLOG_HEADER_ONLY 已由 props 命令行定义（Utils.h 内有 #ifndef 兜底）
#include "Utils.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// ---------- 全局开关（声明在 Utils.h）----------
int consoleOutputLevels[4] = { 1, 1, 1, 1 };
int logToFileEnabled = 1;

// ---------- 文件日志配置（默认值，启动时由 config.ini 的 [Logger] 段覆盖）----------
static size_t maxFileSizeBytes = 10 * 1024 * 1024;   // 单文件 10MB，超过即轮转
static int maxFileCount = 5;                         // 保留最近 5 个轮转文件

// ---------- 异步配置（默认值，启动时由 config.ini 的 [Logger] 段覆盖）----------
static size_t loggerQueueSize = 8192;                // 异步队列长度
static size_t loggerThreadCount = 1;                 // 异步线程池消费线程数

// ---------- 级别映射 ----------
static spdlog::level::level_enum toSpdlogLevel(LogLevel level) {
    switch (level) {
    case LOG_LEVEL_DEBUG: return spdlog::level::debug;
    case LOG_LEVEL_INFO:  return spdlog::level::info;
    case LOG_LEVEL_WARN:  return spdlog::level::warn;
    case LOG_LEVEL_ERROR: return spdlog::level::err;
    default:              return spdlog::level::info;
    }
}

static int levelIndex(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::debug: return LOG_LEVEL_DEBUG;
    case spdlog::level::info:  return LOG_LEVEL_INFO;
    case spdlog::level::warn:  return LOG_LEVEL_WARN;
    case spdlog::level::err:   return LOG_LEVEL_ERROR;
    default:                   return -1;
    }
}

static const char* levelColor(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::debug: return "\033[94m"; // 蓝
    case spdlog::level::info:  return "\033[92m"; // 绿
    case spdlog::level::warn:  return "\033[93m"; // 黄
    case spdlog::level::err:   return "\033[91m"; // 红
    default:                   return "\033[0m";
    }
}

class LevelFilterConsoleSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        int idx = levelIndex(msg.level);
        if (idx < 0) return;

        if (consoleOutputLevels[idx] == 0) return;   // 屏幕是否输出一律看配置（warn/error 同样受控）

        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        const char* color = levelColor(msg.level);
        std::fwrite(color, 1, std::strlen(color), stdout);
        std::fwrite(formatted.data(), 1, formatted.size(), stdout);
        std::fwrite("\033[0m", 1, 4, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    void flush_() override {
        std::fflush(stdout);
    }
};

static std::string logDir = "logs";
static std::shared_ptr<spdlog::logger> logger;       // 进程期唯一 logger，保活到进程退出
static std::atomic<spdlog::logger*> activeLogger{ nullptr };   // 热路径裸指针：initLogger 置一次，closeLogger 置空

static void buildLogger() {
    std::shared_ptr<LevelFilterConsoleSink> consoleSink = std::make_shared<LevelFilterConsoleSink>();
    consoleSink->set_level(spdlog::level::trace);   // 过滤交给开关数组

    std::string filePath = logDir + "/signal.log";
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> fileSink =
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filePath, maxFileSizeBytes, maxFileCount);
    fileSink->set_level(logToFileEnabled != 0 ? spdlog::level::trace : spdlog::level::off);

    logger = std::make_shared<spdlog::async_logger>(
        "webrtc-signal",
        spdlog::sinks_init_list{ consoleSink, fileSink },
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(spdlog::level::trace);        // 级别过滤交给各 sink
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l] %s:%# %v");

    activeLogger.store(logger.get(), std::memory_order_release);   // 发布唯一 logger，热路径原子可见

    spdlog::flush_every(std::chrono::seconds(3));

    spdlog::set_default_logger(logger);
}

void initLogger() {
    if (logger) return;                             // 幂等；启动期单线程调用，无需加锁
    spdlog::init_thread_pool(loggerQueueSize, loggerThreadCount);   // 队列长度与消费线程数来自 config.ini
    buildLogger();
}

void closeLogger() {
    if (!logger) return;
    logger->flush();                                // 等待待写消息落地
    activeLogger.store(nullptr, std::memory_order_release);         // 先摘热路径指针，之后的 logMessage 直接秒退
    spdlog::shutdown();                             // 冲刷并停掉异步线程池
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

void setFileLoggingConfig(int enable, const char* directory, int maxFileSizeMB, int fileCount) {
    logToFileEnabled = enable;
    if (directory != nullptr && directory[0] != '\0') logDir = directory;
    if (maxFileSizeMB > 0) maxFileSizeBytes = static_cast<size_t>(maxFileSizeMB) * 1024 * 1024;
    if (fileCount > 0) maxFileCount = fileCount;
}

void setLoggerAsyncConfig(int queueSize, int threadCount) {
    if (queueSize > 0) loggerQueueSize = static_cast<size_t>(queueSize);
    if (threadCount > 0) loggerThreadCount = static_cast<size_t>(threadCount);
}

void setLogDirectory(const char* dir) {
    logDir = dir ? dir : "logs";
}

// ---------- 日志入口（宏 → fmt 格式化后到此；被关闭的级别在宏处短路，进不来）----------
void hope::log::logMessage(LogLevel level, const char* file, int line, const std::string& message) {
    spdlog::logger* currentLogger = activeLogger.load(std::memory_order_acquire);
    if (!currentLogger) return;            
    currentLogger->log(spdlog::source_loc{ file, line, "" }, toSpdlogLevel(level), "{}", message);
}
