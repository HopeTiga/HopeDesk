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

#include <mimalloc/mimalloc-new-delete.h>

#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <thread>
#include <boost/asio.hpp>

#include "system/WinLogon.h"
#include "system/SessionHelper.h"
#include "utils/Utils.h"
#include "rtc/WebrtcManager.h"

SERVICE_STATUS serviceStatus = { 0 };
SERVICE_STATUS_HANDLE statusHandle = NULL;
HANDLE stopEvent = NULL;
std::atomic<bool> isRespawnedProcess(false);

std::string WstringToString(const std::wstring& wstr) {

    if (wstr.empty()) return std::string();

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);

    std::string strTo(sizeNeeded, 0);

    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);

    return strTo;
}

std::wstring StringToWstring(const std::string& str) {

    if (str.empty()) return std::wstring();

    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);

    std::wstring wstrTo(sizeNeeded, 0);

    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], sizeNeeded);

    return wstrTo;
}

DWORD GetCurrentSessionId() {

    DWORD sessionId = 0;

    DWORD processId = GetCurrentProcessId();

    ProcessIdToSessionId(processId, &sessionId);

    return sessionId;

}

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

        LOG_INFO("Current User: {} | SessionID: {} | Process Type: {}",
            WstringToString(username).c_str(), sessionId, GetProcessTypeString().c_str());

        return wcscmp(username, L"SYSTEM") == 0;

    }

    LOG_ERROR("Failed To Get Username");

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

void AsyncEvent() {

    DWORD sessionId = GetCurrentSessionId();

    std::string processType = GetProcessTypeString();

    boost::asio::io_context ioContext;

    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> ioContextWorkPtr =
        std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(ioContext));

    std::shared_ptr<hope::rtc::WebrtcManager> webrtcManager = std::make_shared<hope::rtc::WebrtcManager>([&ioContext, &ioContextWorkPtr]() {

        ioContextWorkPtr.reset();

        ioContext.stop();

        });

    LOG_INFO("AsyncEvent Start");

    initLogger();

    ioContext.run();

    closeLogger();

    closeWebrtcLogging();
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {

    DWORD sessionId = GetCurrentSessionId();

    std::string serviceName = (argc > 0 && argv && argv[0]) ? WstringToString(argv[0]) : "";

    LOG_INFO("[MAINPROCESS] Service Starting | SessionID: {} | Service: {}", sessionId, serviceName.c_str());

    statusHandle = RegisterServiceCtrlHandlerA(serviceName.c_str(), ServiceCtrlHandler);

    if (!statusHandle) {

        LOG_ERROR("[MAINPROCESS] SessionID: {} - Failed To Register Service Control Handler", sessionId);

        return;

    }

    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    serviceStatus.dwCurrentState = SERVICE_START_PENDING;

    SetServiceStatus(statusHandle, &serviceStatus);

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!stopEvent) {

        LOG_ERROR("[MAINPROCESS] SessionID: {} - Failed To Create Stop Event", sessionId);

        return;

    }

    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    serviceStatus.dwCurrentState = SERVICE_RUNNING;

    SetServiceStatus(statusHandle, &serviceStatus);

    LOG_INFO("[MAINPROCESS] SessionID: {} - Service Started Successfully", sessionId);

    if (!IsRunningAsSystem()) {

        LOG_ERROR("[MAINPROCESS] SessionID: {} - This Service Must Be Run As 'NT AUTHORITY\\SYSTEM'", sessionId);

        serviceStatus.dwCurrentState = SERVICE_STOPPED;

        SetServiceStatus(statusHandle, &serviceStatus);

        return;

    }

    HANDLE process = nullptr;

    if (!hope::system::SessionHelper::CheckActiveTerminalSession()) {

        LOG_INFO("[MAINPROCESS] SessionID: {} - Service Running In Session 0, Respawning In Active Session...", sessionId);

        try {

            process = hope::system::SessionHelper::CreateSystemProcessInUserSession(L"--respawned");

            LOG_INFO("[MAINPROCESS] SessionID: {} - Respawned Process In Active Session", sessionId);

            CloseHandle(process);

            process = nullptr;

        }
        catch (const std::exception& e) {

            LOG_ERROR("[MAINPROCESS] SessionID: {} - Failed To Respawn: {}", sessionId, e.what());

        }
    }
    else {

        LOG_WARN("[MAINPROCESS] SessionID: {} - Service Unexpectedly In Active Session", sessionId);

    }

    CloseHandle(stopEvent);

    serviceStatus.dwCurrentState = SERVICE_STOPPED;

    SetServiceStatus(statusHandle, &serviceStatus);

    // The worker is intentionally left running; this process exits on purpose.
    LOG_INFO("[MAINPROCESS] SessionID: {} - Service Exiting After Spawning Worker", sessionId);
}

int main(int argc, char* argv[]) {

    mi_version();

    DWORD sessionId = GetCurrentSessionId();

    if (IsRespawnedProcess(argc, argv)) {

        isRespawnedProcess = true;

        LOG_INFO("[SUBPROCESS] Running As Respawned Process With DXGI | SessionID: {}", sessionId);

        if (!IsRunningAsSystem()) {

            LOG_ERROR("[SUBPROCESS] SessionID: {} - Respawned Process Must Run As SYSTEM", sessionId);

            return 1;

        }

        AsyncEvent();

        return 0;
    }

    LOG_INFO("[MAINPROCESS] Starting Service Dispatcher With DXGI Support | SessionID: {}", sessionId);

    std::string serviceName = (argc > 1) ? argv[1] : "";
    std::wstring serviceNameWide = StringToWstring(serviceName);

    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { serviceNameWide.data(), ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {

        DWORD error = GetLastError();

        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {

            LOG_INFO("[MAINPROCESS] SessionID: {} - Running In Interactive Mode With DXGI (Not As Service)", sessionId);

            if (!IsRunningAsSystem()) {

                LOG_ERROR("[MAINPROCESS] SessionID: {} - Must Run As SYSTEM User", sessionId);

                return 1;

            }
        }
        else {

            LOG_ERROR("[MAINPROCESS] SessionID: {} - Failed To Start Service Dispatcher: {}", sessionId, error);

            return 1;

        }
    }

    return 0;
}