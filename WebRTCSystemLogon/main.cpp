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
#include "Utils.h"
#include "WebRTCManager.h"

#define SERVICE_NAME "WebRTCSystemLogon"

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
        LOG_INFO("Current user: %s | SessionID: %d | Process Type: %s",
            WstringToString(username).c_str(), sessionId, GetProcessTypeString().c_str());
        return wcscmp(username, L"SYSTEM") == 0;
    }
    LOG_ERROR("Failed to get username");
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
    DWORD sessionId = GetCurrentSessionId();
    std::string processType = GetProcessTypeString();

    std::unique_ptr<hope::rtc::WebRTCManager> webrtcManager = std::make_unique<hope::rtc::WebRTCManager>();

    boost::asio::io_context ioContext;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr =
        std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    LOG_INFO("RunSystemLoop start");

    webrtcManager->stopProcessCallBack = [&ioContext, sessionId, &webrtcManager]() mutable {
        LOG_INFO("[SUBPROCESS] SessionID: %d - Stopping WebRTC Remote and exiting...", sessionId);
        ioContext.stop();
        };

    ioContext.run();
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    DWORD sessionId = GetCurrentSessionId();
    LOG_INFO("[MAINPROCESS] Service starting | SessionID: %d", sessionId);

    statusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, ServiceCtrlHandler);
    if (!statusHandle) {
        LOG_ERROR("[MAINPROCESS] SessionID: %d - Failed to register service control handler", sessionId);
        return;
    }

    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(statusHandle, &serviceStatus);

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stopEvent) {
        LOG_ERROR("[MAINPROCESS] SessionID: %d - Failed to create stop event", sessionId);
        return;
    }

    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(statusHandle, &serviceStatus);

    LOG_INFO("[MAINPROCESS] SessionID: %d - Service started successfully", sessionId);

    if (!IsRunningAsSystem()) {
        LOG_ERROR("[MAINPROCESS] SessionID: %d - This service must be run as 'NT AUTHORITY\\SYSTEM'", sessionId);
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(statusHandle, &serviceStatus);
        return;
    }

    HANDLE process = nullptr;

    if (!hope::rtc::SessionHelper::CheckActiveTerminalSession()) {
        LOG_INFO("[MAINPROCESS] SessionID: %d - Service running in Session 0, respawning in active session...", sessionId);

        try {
            process = hope::rtc::SessionHelper::CreateSystemProcessInUserSession(L"--respawned");
            LOG_INFO("[MAINPROCESS] SessionID: %d - Respawned process in active session", sessionId);
            WaitForSingleObject(stopEvent, INFINITE);
        }
        catch (const std::exception& e) {
            LOG_ERROR("[MAINPROCESS] SessionID: %d - Failed to respawn: %s", sessionId, e.what());
        }
    }
    else {
        LOG_WARNING("[MAINPROCESS] SessionID: %d - Service unexpectedly in active session", sessionId);
    }

    CloseHandle(stopEvent);
    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(statusHandle, &serviceStatus);

    if (process != nullptr) {
        DWORD exitCode;
        if (GetExitCodeProcess(process, &exitCode)) {
            if (exitCode == STILL_ACTIVE) {
                LOG_INFO("ProcessManager: Child process still running, forcing termination");

                if (!TerminateProcess(process, 1)) {
                    LOG_ERROR("ProcessManager: Force termination failed");
                }
                else {
                    LOG_INFO("ProcessManager: Child process has been force terminated");
                }
            }
        }

        CloseHandle(process);
    }

    LOG_INFO("[MAINPROCESS] SessionID: %d - Service stopped successfully", sessionId);
}

int main(int argc, char* argv[]) {
    DWORD sessionId = GetCurrentSessionId();

    if (IsRespawnedProcess(argc, argv)) {
        isRespawnedProcess = true;
        LOG_INFO("[SUBPROCESS] Running as respawned process with DXGI | SessionID: %d", sessionId);

        if (!IsRunningAsSystem()) {
            LOG_ERROR("[SUBPROCESS] SessionID: %d - Respawned process must run as SYSTEM", sessionId);
            return 1;
        }

        RunSystemLoop();
        return 0;
    }

    LOG_INFO("[MAINPROCESS] Starting service dispatcher with DXGI support | SessionID: %d", sessionId);

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            LOG_INFO("[MAINPROCESS] SessionID: %d - Running in interactive mode with DXGI (not as service)", sessionId);

            if (!IsRunningAsSystem()) {
                LOG_ERROR("[MAINPROCESS] SessionID: %d - Must run as SYSTEM user", sessionId);
                return 1;
            }
        }
        else {
            LOG_ERROR("[MAINPROCESS] SessionID: %d - Failed to start service dispatcher: %d", sessionId, error);
            return 1;
        }
    }

    return 0;
}