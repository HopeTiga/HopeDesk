#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   // 阻止 windows.h 包含 winsock.h
#endif

#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <thread>
#include <boost/asio.hpp>

// 项目头文件
#include "WinLogon.h"
#include "SessionHelper.h"
#include "Logger.h"
#include "WebRTCManager.h"

#define SERVICE_NAME "WinlogonCaptureService2"

SERVICE_STATUS serviceStatus = { 0 };
SERVICE_STATUS_HANDLE statusHandle = NULL;
HANDLE stopEvent = NULL;
std::atomic<bool> isRespawnedProcess(false);


// 宽字符串转窄字符串 - 使用 Windows API
std::string WstringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

// 获取当前进程的SessionID
DWORD GetCurrentSessionId() {
    DWORD sessionId = 0;
    DWORD processId = GetCurrentProcessId();
    ProcessIdToSessionId(processId, &sessionId);
    return sessionId;
}

// 获取进程类型字符串（主进程或子进程）
std::string GetProcessTypeString() {
    return isRespawnedProcess ? "SUBPROCESS" : "MAINPROCESS";
}

VOID WINAPI ServiceCtrlHandler(DWORD ctrlCode) {
    if (ctrlCode == SERVICE_CONTROL_STOP) {
        serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(statusHandle, &serviceStatus);
        SetEvent(stopEvent);
    }
}

bool IsRunningAsSystem() {
    wchar_t username[256];
    DWORD size = sizeof(username) / sizeof(username[0]);
    if (GetUserNameW(username, &size)) {
        DWORD sessionId = GetCurrentSessionId();
        Logger::getInstance()->info(
            std::string("Current user: ") + WstringToString(username) +
            " | SessionID: " + std::to_string(sessionId) +
            " | Process Type: " + GetProcessTypeString()
        );
        return wcscmp(username, L"SYSTEM") == 0;
    }
    Logger::getInstance()->error("Failed to get username");
    return false;
}

bool IsRespawnedProcess(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--respawned") == 0) {
            return true;
        }
    }
    return false;
}

void RunSystemLoop() {

    Logger* logger = Logger::getInstance();

    DWORD sessionId = GetCurrentSessionId();

    std::string processType = GetProcessTypeString();

    std::unique_ptr<WebRTCManager> webrtcManager = std::make_unique<WebRTCManager>(WebRTCRemoteState::nullRemote);

    boost::asio::io_context ioContext;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    logger->info("RunSystemLoop start");

    webrtcManager->stopProcessCallBack = [&ioContext, &logger, sessionId, &webrtcManager]()mutable {
        logger->info("[SUBPROCESS] SessionID: " + std::to_string(sessionId) +
            " - Stopping WebRTC Remote and exiting...");
        ioContext.stop();

        };

    ioContext.run();

}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    Logger* logger = Logger::getInstance();
    logger->setLogLevels(LogLevels::INFO);

    DWORD sessionId = GetCurrentSessionId();
    logger->info("[MAINPROCESS] Service starting | SessionID: " + std::to_string(sessionId));

    statusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, ServiceCtrlHandler);
    if (!statusHandle) {
        logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
            " - Failed to register service control handler");
        return;
    }

    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(statusHandle, &serviceStatus);

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stopEvent) {
        logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
            " - Failed to create stop event");
        return;
    }

    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(statusHandle, &serviceStatus);

    logger->info("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
        " - Service started successfully");

    if (!IsRunningAsSystem()) {
        logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
            " - This service must be run as 'NT AUTHORITY\\SYSTEM'");
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(statusHandle, &serviceStatus);
        return;
    }

    HANDLE process = nullptr;

    if (!SessionHelper::CheckActiveTerminalSession()) {
        logger->info("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
            " - Service running in Session 0, respawning in active session...");

        try {
            process = SessionHelper::CreateSystemProcessInUserSession(L"--respawned");
            logger->info("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
                " - Respawned process in active session");
            WaitForSingleObject(stopEvent, INFINITE);
        }
        catch (const std::exception& e) {
            logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
                " - Failed to respawn: " + e.what());
        }
    }
    else {
        logger->warning("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
            " - Service unexpectedly in active session");
    }

    CloseHandle(stopEvent);
    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(statusHandle, &serviceStatus);

    if (process != nullptr) {
        DWORD exitCode;
        if (GetExitCodeProcess(process, &exitCode)) {
            if (exitCode == STILL_ACTIVE) {
                Logger::getInstance()->info("ProcessManager: 子进程仍在运行，强制终止");

                if (!TerminateProcess(process, 1)) {
                    Logger::getInstance()->error("ProcessManager: 强制终止失败");
                }
                else {
                    Logger::getInstance()->info("ProcessManager: 子进程已强制终止");
                }
            }
        }

        CloseHandle(process);
    }

    logger->info("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
        " - Service stopped successfully");
}

int main(int argc, char* argv[]) {
    Logger* logger = Logger::getInstance();
    logger->setLogLevels(LogLevels::INFO);

    DWORD sessionId = GetCurrentSessionId();

    if (IsRespawnedProcess(argc, argv)) {
        isRespawnedProcess = true;
        logger->info("[SUBPROCESS] Running as respawned process with DXGI | SessionID: " +
            std::to_string(sessionId));

        if (!IsRunningAsSystem()) {
            logger->error("[SUBPROCESS] SessionID: " + std::to_string(sessionId) +
                " - Respawned process must run as SYSTEM");
            return 1;
        }

        RunSystemLoop();
        return 0;
    }

    logger->info("[MAINPROCESS] Starting service dispatcher with DXGI support | SessionID: " +
        std::to_string(sessionId));

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            logger->info("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
                " - Running in interactive mode with DXGI (not as service)");

            if (!IsRunningAsSystem()) {
                logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
                    " - Must run as SYSTEM user");
                return 1;
            }
        }
        else {
            logger->error("[MAINPROCESS] SessionID: " + std::to_string(sessionId) +
                " - Failed to start service dispatcher: " + std::to_string(error));
            return 1;
        }
    }

    return 0;
}