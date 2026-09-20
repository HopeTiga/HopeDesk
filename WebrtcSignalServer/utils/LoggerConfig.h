#pragma once
#ifndef LOGGER_CONFIG_H
#define LOGGER_CONFIG_H

#include <string>

#include "ConfigManager.h"
#include "Utils.h"

namespace hope {
namespace utils {

struct LoggerConfig {

    int queueSize = 8192;
    int threadCount = 1;
    int logToFile = 1;
    std::string logDirectory = "logs";
    int maxFileSizeMB = 10;
    int maxFiles = 5;
    // 键是大写的(Logger.DEBUG 等),成员名小写,键名不改免得已有 config.ini 失效
    int debug = 0;
    int info = 1;
    int warn = 1;
    int error = 0;

};

inline void loadLoggerConfig(LoggerConfig& loggerConfig, const ConfigManager& configManager) {

    loggerConfig.queueSize = configManager.GetInt("Logger.queueSize", loggerConfig.queueSize);
    loggerConfig.threadCount = configManager.GetInt("Logger.threadCount", loggerConfig.threadCount);
    loggerConfig.logToFile = configManager.GetInt("Logger.logToFile", loggerConfig.logToFile);
    loggerConfig.logDirectory = configManager.GetString("Logger.logDirectory", loggerConfig.logDirectory);
    loggerConfig.maxFileSizeMB = configManager.GetInt("Logger.maxFileSizeMB", loggerConfig.maxFileSizeMB);
    loggerConfig.maxFiles = configManager.GetInt("Logger.maxFiles", loggerConfig.maxFiles);
    loggerConfig.debug = configManager.GetInt("Logger.DEBUG", loggerConfig.debug);
    loggerConfig.info = configManager.GetInt("Logger.INFO", loggerConfig.info);
    loggerConfig.warn = configManager.GetInt("Logger.WARN", loggerConfig.warn);
    loggerConfig.error = configManager.GetInt("Logger.ERROR", loggerConfig.error);

}

inline void applyLoggerConfig(const LoggerConfig& loggerConfig) {

    setLoggerAsyncConfig(loggerConfig.queueSize, loggerConfig.threadCount);
    setFileLoggingConfig(loggerConfig.logToFile, loggerConfig.logDirectory.c_str(), loggerConfig.maxFileSizeMB, loggerConfig.maxFiles);
    initLogger();
    setConsoleOutputLevels(loggerConfig.debug, loggerConfig.info, loggerConfig.warn, loggerConfig.error);

}

} // namespace utils
} // namespace hope

#endif // LOGGER_CONFIG_H
