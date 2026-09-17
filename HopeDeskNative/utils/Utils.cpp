#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
// WebRTC 头会拉 winsock2.h;必须先于 Utils.h 加载 winsock2.h,否则
// Utils.h 里的 windows.h 先加载 winsock.h -> sendto/sockaddr 重定义。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <share.h>
#endif

#include "Utils.h"

// 完整 spdlog（header-only）只在本 TU 编译；Utils.h 里只有 fmt，供 LOG_* 宏做编译期格式校验
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <chrono>
#include <mutex>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdio>
#include "rtc_base/logging.h"   // webrtc 内部 RTC_LOG:排查 ICE/DTLS 用

static const char* COLOR_RESET = "\033[0m";
static const char* COLOR_RED = "\033[91m";
static const char* COLOR_GREEN = "\033[92m";
static const char* COLOR_YELLOW = "\033[93m";
static const char* COLOR_BLUE = "\033[94m";

static std::string logDir = "logs";
// 下面两个在 Utils.h 里是 extern（供 LOG_* 宏在调用点短路级别），必须外部链接，不能加 static
int logToFileEnabled = 1;
int consoleOutputLevels[4] = { 1, 1, 1, 1 };

static size_t maxFileSizeBytes = 10 * 1024 * 1024;   // 单文件 10MB，超过即轮转
static int maxFileCount = 5;                         // 保留最近 5 个轮转文件

static std::mutex loggerMutex;                       // 只保护建/换 logger，不在写路径上
static std::shared_ptr<spdlog::logger> logger;                  // 进程期唯一 logger
static std::shared_ptr<spdlog::logger> fileOnlyLogger;          // logToFileOnly 用的"只写文件"logger
static std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> fileSink;
static std::atomic<spdlog::logger*> activeLogger{ nullptr };         // 写路径取它，不每次加锁
static std::atomic<spdlog::logger*> activeFileOnlyLogger{ nullptr };

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
    case spdlog::level::debug: return COLOR_BLUE;
    case spdlog::level::info:  return COLOR_GREEN;
    case spdlog::level::warn:  return COLOR_YELLOW;
    case spdlog::level::err:   return COLOR_RED;
    default:                   return COLOR_RESET;
    }
}

// 控制台 sink：逐级别开关(consoleOutputLevels)与着色都在这里，不借 spdlog 自己的 level 过滤
class LevelFilterConsoleSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        int idx = levelIndex(msg.level);
        if (idx < 0) return;
        if (consoleOutputLevels[idx] == 0) return;

        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        const char* color = levelColor(msg.level);
        std::fwrite(color, 1, std::strlen(color), stdout);
        std::fwrite(formatted.data(), 1, formatted.size(), stdout);
        std::fwrite(COLOR_RESET, 1, std::strlen(COLOR_RESET), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    void flush_() override {
        std::fflush(stdout);
    }
};

static void ensureLogDirectory() {
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
}

static void buildLogger() {
    std::shared_ptr<LevelFilterConsoleSink> consoleSink = std::make_shared<LevelFilterConsoleSink>();
    consoleSink->set_level(spdlog::level::trace);   // 屏幕过滤交给 consoleOutputLevels

    std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks{ consoleSink };

    fileSink.reset();
    try {
        fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logDir + "/HopeDeskNative.log", maxFileSizeBytes, maxFileCount);
        fileSink->set_level(logToFileEnabled != 0 ? spdlog::level::trace : spdlog::level::off);
        sinks.push_back(fileSink);
    }
    catch (const spdlog::spdlog_ex& e) {
        // 打不开就退化成只有控制台：写日志不能抛出去（旧实现是静默丢，不抛）
        fprintf(stderr, "ERROR: Failed to open log file under %s: %s\n", logDir.c_str(), e.what());
    }

    logger = std::make_shared<spdlog::logger>("HopeDeskNative", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);        // 级别过滤交给各 sink
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l] %s:%# %v");
    logger->flush_on(spdlog::level::trace);         // 每条都 flush：保持"写出去就落盘"

    activeLogger.store(logger.get(), std::memory_order_release);

    if (fileSink) {
        fileOnlyLogger = std::make_shared<spdlog::logger>("HopeDeskNative.file", fileSink);
        fileOnlyLogger->set_pattern("[%Y-%m-%d %H:%M:%S.%e][%l] %s:%# %v");
        fileOnlyLogger->flush_on(spdlog::level::trace);
        activeFileOnlyLogger.store(fileOnlyLogger.get(), std::memory_order_release);
    }
}

// 首次写日志时惰性建好（Native 从不调 initLogger，靠这里）
static void ensureLoggerInitialized() {
    if (activeLogger.load(std::memory_order_acquire) != nullptr) return;
    std::lock_guard<std::mutex> lock(loggerMutex);
    if (logger) return;
    ensureLogDirectory();
    buildLogger();
}

void initLogger() {
    ensureLoggerInitialized();
}

void closeLogger() {
    std::lock_guard<std::mutex> lock(loggerMutex);
    activeLogger.store(nullptr, std::memory_order_release);
    activeFileOnlyLogger.store(nullptr, std::memory_order_release);
    if (logger) {
        logger->flush();
        logger.reset();
    }
    fileOnlyLogger.reset();
    fileSink.reset();
}

// ===== WebRTC 内部日志(RTC_LOG)=====
// 把 libwebrtc 的 RTC_LOG 接到 logs/webrtc.log,用于排查 ICE/DTLS/连接建立。
// LS_VERBOSE 最详细(能看到 ICE 连通性检查、candidate pair 状态),日志量大。
namespace {

class WebrtcLogSink : public webrtc::LogSink {
public:
    explicit WebrtcLogSink(const std::string& path) : file(path, std::ios::out | std::ios::app) {}

    void write(const std::string& message) {
        std::lock_guard<std::mutex> lock(mtx);
        if (file.is_open()) file << message << '\n';
    }

    // 覆写所有重载:不管 libwebrtc 从哪个入口(带 severity/tag、string_view、LogLineRef)派发,
    // 都能落到文件。基类其它重载的默认实现可能是空的,只覆写纯虚函数有收不到日志的风险。
    void OnLogMessage(const std::string& msg, webrtc::LoggingSeverity severity, const char* tag) override { write(msg); }
    void OnLogMessage(const std::string& message, webrtc::LoggingSeverity severity) override { write(message); }
    void OnLogMessage(const std::string& message) override { write(message); }
    void OnLogMessage(absl::string_view msg, webrtc::LoggingSeverity severity, const char* tag) override { write(std::string(msg)); }
    void OnLogMessage(absl::string_view message, webrtc::LoggingSeverity severity) override { write(std::string(message)); }
    void OnLogMessage(absl::string_view message) override { write(std::string(message)); }
    void OnLogMessage(const webrtc::LogLineRef& line) override { write(std::string(line.message())); }

    void flush() {
        std::lock_guard<std::mutex> lock(mtx);
        if (file.is_open()) file.flush();
    }

private:
    std::ofstream file;
    std::mutex mtx;
};

}  // namespace

static WebrtcLogSink* gWebrtcLogSink = nullptr;

void initWebrtcLogging() {
    if (gWebrtcLogSink) return;
    ensureLogDirectory();
#ifdef _WIN32
    std::string path = logDir + "\\webrtc.log";
#else
    std::string path = logDir + "/webrtc.log";
#endif
    gWebrtcLogSink = new WebrtcLogSink(path);
    webrtc::LogMessage::LogTimestamps();
    webrtc::LogMessage::AddLogToStream(gWebrtcLogSink, webrtc::LS_VERBOSE);
}

void closeWebrtcLogging() {
    if (!gWebrtcLogSink) return;
    gWebrtcLogSink->flush();
    webrtc::LogMessage::RemoveLogToStream(gWebrtcLogSink);
    delete gWebrtcLogSink;
    gWebrtcLogSink = nullptr;
}

void enableFileLogging(int enable) {
    logToFileEnabled = enable;
    if (fileSink) fileSink->set_level(enable != 0 ? spdlog::level::trace : spdlog::level::off);
}

void setLogDirectory(const char* dir) {
    std::lock_guard<std::mutex> lock(loggerMutex);
    logDir = dir ? dir : "logs";
    activeLogger.store(nullptr, std::memory_order_release);
    activeFileOnlyLogger.store(nullptr, std::memory_order_release);
    if (logger) logger->flush();
    logger.reset();
    fileOnlyLogger.reset();
    fileSink.reset();
    ensureLogDirectory();
    buildLogger();      // 按新目录立刻建好，和旧实现"立刻重开文件"一致
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

// 日志入口：LOG_* 宏已在调用点用 fmt 完成校验与格式化，这里只负责落到 sink
void hope::log::logMessage(LogLevel level, const char* file, int line, const std::string& message, bool fileOnly) {
    if (fileOnly) {
        spdlog::logger* target = activeFileOnlyLogger.load(std::memory_order_acquire);
        if (target) target->log(spdlog::source_loc{ file, line, "" }, toSpdlogLevel(level), "{}", message);
        return;
    }

    spdlog::logger* currentLogger = activeLogger.load(std::memory_order_acquire);
    if (currentLogger == nullptr) {
        ensureLoggerInitialized();
        currentLogger = activeLogger.load(std::memory_order_acquire);
        if (currentLogger == nullptr) return;
    }
    currentLogger->log(spdlog::source_loc{ file, line, "" }, toSpdlogLevel(level), "{}", message);
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
