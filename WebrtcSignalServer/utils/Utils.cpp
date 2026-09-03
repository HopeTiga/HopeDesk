
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
#include <vector>
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

// ---------- 日志器状态 ----------
static std::string logDir = "logs";
static std::mutex loggerMutex;                       // 只保护 初始化/重建/关闭，热路径不碰
static std::shared_ptr<spdlog::logger> logger;       // 最新 logger（重建时换出保活用）
static std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> fileSink;
static std::atomic<spdlog::logger*> activeLogger{ nullptr };          // 热路径裸指针，logMessage 只读它
static std::vector<std::shared_ptr<spdlog::logger>> retiredLoggers;   // 保活换出的旧 logger：在途 logMessage 可能仍握着它的裸指针

// 调用方必须已持有 loggerMutex
static void buildLogger() {
    std::shared_ptr<LevelFilterConsoleSink> consoleSink = std::make_shared<LevelFilterConsoleSink>();
    consoleSink->set_level(spdlog::level::trace);   // 过滤交给开关数组

    std::string filePath = logDir + "/signal.log";
    fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filePath, maxFileSizeBytes, maxFileCount);
    fileSink->set_level(logToFileEnabled != 0 ? spdlog::level::trace : spdlog::level::off);

    if (logger) {
        // 旧 logger 换出后只保活不析构：此刻可能有线程已加载它的裸指针、正要去 log()
        retiredLoggers.push_back(std::move(logger));
    }

    logger = std::make_shared<spdlog::async_logger>(
        "webrtc-signal",
        spdlog::sinks_init_list{ consoleSink, fileSink },
        spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    logger->set_level(spdlog::level::trace);        // 级别过滤交给各 sink
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l] %s:%# %v");

    activeLogger.store(logger.get(), std::memory_order_release);   // 发布新 logger，热路径原子可见

    spdlog::flush_every(std::chrono::seconds(3));

    spdlog::set_default_logger(logger);
}

// ---------- 公开接口 ----------
void initLogger() {
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (logger) return;                             // 幂等
    spdlog::init_thread_pool(loggerQueueSize, loggerThreadCount);   // 队列长度与消费线程数来自 config.ini
    buildLogger();
}

void closeLogger() {
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (!logger) return;
    logger->flush();                                // 等待待写消息落地
    spdlog::shutdown();                             // 冲刷并停掉异步线程池
    activeLogger.store(nullptr, std::memory_order_release);   // 置空热路径指针，之后的 logMessage 直接秒退
    retiredLoggers.push_back(std::move(logger));    // 保活当前 logger：在途 logMessage 可能仍握着它的裸指针
}

void enableFileLogging(int enable) {
    logToFileEnabled = enable;
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (fileSink) {
        fileSink->set_level(enable != 0 ? spdlog::level::trace : spdlog::level::off);
    }
}

void setConsoleOutputLevels(int debug, int info, int warn, int error) {
    consoleOutputLevels[LOG_LEVEL_DEBUG] = debug;
    consoleOutputLevels[LOG_LEVEL_INFO] = info;
    consoleOutputLevels[LOG_LEVEL_WARN] = warn;
    consoleOutputLevels[LOG_LEVEL_ERROR] = error;
}

// 一次性应用 config.ini [Logger] 段：文件日志开关、目录、单文件大小(MB)、轮转个数
void setFileLoggingConfig(int enable, const char* directory, int maxFileSizeMB, int fileCount) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    logToFileEnabled = enable;
    if (directory != nullptr && directory[0] != '\0') logDir = directory;
    if (maxFileSizeMB > 0) maxFileSizeBytes = static_cast<size_t>(maxFileSizeMB) * 1024 * 1024;
    if (fileCount > 0) maxFileCount = fileCount;
    if (logger) {
        // 大小/目录变化需重建轮转 sink（旧 sink 持有已打开的文件句柄）
        logger->flush();
        spdlog::drop("webrtc-signal");              // 从注册表移除旧名字；旧 logger 交给 buildLogger 退休保活
        buildLogger();
    }
}

// 应用 config.ini [Logger] 段的异步配置：队列长度、消费线程数
// 线程池在 initLogger() 时创建，所以该函数必须在 initLogger() 之前调用
void setLoggerAsyncConfig(int queueSize, int threadCount) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (queueSize > 0) loggerQueueSize = static_cast<size_t>(queueSize);
    if (threadCount > 0) loggerThreadCount = static_cast<size_t>(threadCount);
}

void setLogDirectory(const char* dir) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    logDir = dir ? dir : "logs";
    if (logger) {
        // 切换目录需重建文件 sink（旧 sink 持有已打开的文件句柄）
        logger->flush();
        spdlog::drop("webrtc-signal");              // 从注册表移除旧名字；旧 logger 交给 buildLogger 退休保活
        buildLogger();
    }
}

// ---------- 日志入口（宏 → fmt 格式化后到此）----------
void hope::log::logMessage(LogLevel level, const char* file, int line, const std::string& message) {
    // 热路径只做一次裸指针 acquire-load：不碰锁、不拷贝 shared_ptr
    spdlog::logger* currentLogger = activeLogger.load(std::memory_order_acquire);
    if (!currentLogger) return;                     // closeLogger 之后安全丢弃
    currentLogger->log(spdlog::source_loc{ file, line, "" }, toSpdlogLevel(level), "{}", message);
}
