#include "WinLogon.h"
#include <stdexcept>
#include <string>
#include <tchar.h>

// 定义Windows API常量
#define WINSTA_ALL_ACCESS 0x37F
#define DESKTOP_ALL_ACCESS 0x01FF

WinLogon::WinLogon() {
    SwitchToWinLogonDesktop();
}

WinLogon::~WinLogon() {
    Dispose(false);
}

void WinLogon::SwitchToWinLogonDesktop() {
    // 如果已经在安全桌面，直接返回
    if (_isOnWinLogonDesktop) {
        return;
    }

    // 打开WinSta0窗口站
    _winSta0Station = OpenWindowStation(TEXT("WinSta0"), FALSE, WINSTA_ALL_ACCESS);
    if (_winSta0Station == NULL) {
        throw std::runtime_error("Failed to open WinSta0 window station: " + std::to_string(GetLastError()));
    }

    // 保存当前窗口站
    _oldProcessWinStation = GetProcessWindowStation();

    // 设置新的窗口站
    if (!SetProcessWindowStation(_winSta0Station)) {
        CloseWindowStation(_winSta0Station);
        throw std::runtime_error("Failed to set process window station: " + std::to_string(GetLastError()));
    }

    // 打开winlogon桌面
    _winLogonDesktop = OpenDesktop(TEXT("winlogon"), 0, FALSE, DESKTOP_ALL_ACCESS);
    if (_winLogonDesktop == NULL) {
        SetProcessWindowStation(_oldProcessWinStation);
        CloseWindowStation(_winSta0Station);
        throw std::runtime_error("Failed to open winlogon desktop: " + std::to_string(GetLastError()));
    }

    // 保存当前线程桌面
    _oldThreadDesktop = GetThreadDesktop(GetCurrentThreadId());

    // 设置新的线程桌面
    if (!SetThreadDesktop(_winLogonDesktop)) {
        CloseDesktop(_winLogonDesktop);
        SetProcessWindowStation(_oldProcessWinStation);
        CloseWindowStation(_winSta0Station);
        throw std::runtime_error("Failed to set thread desktop: " + std::to_string(GetLastError()));
    }

    _isOnWinLogonDesktop = true;
}

bool WinLogon::IsCurrentlyOnSecureDesktop() {
    HDESK currentDesktop = GetThreadDesktop(GetCurrentThreadId());
    if (currentDesktop == NULL) {
        return false;
    }

    // 获取桌面名称
    TCHAR desktopName[256] = { 0 };
    DWORD needed = 0;

    if (!GetUserObjectInformation(currentDesktop, UOI_NAME,
        desktopName, sizeof(desktopName),
        &needed)) {
        return false;
    }

    // 比较桌面名称
    // winlogon 是安全桌面，返回 true
    // Default 是普通桌面，返回 false
    return (_tcsicmp(desktopName, TEXT("winlogon")) == 0);
}

void WinLogon::SwitchToDefaultDesktop() {
    // 如果不在安全桌面，直接返回
    if (!_isOnWinLogonDesktop) {
        return;
    }

    // 确保WinSta0窗口站是打开的
    if (_winSta0Station == NULL) {
        _winSta0Station = OpenWindowStation(TEXT("WinSta0"), FALSE, WINSTA_ALL_ACCESS);
        if (_winSta0Station == NULL) {
            throw std::runtime_error("Failed to open WinSta0 window station: " + std::to_string(GetLastError()));
        }
    }

    // 设置窗口站
    if (!SetProcessWindowStation(_winSta0Station)) {
        throw std::runtime_error("Failed to set process window station: " + std::to_string(GetLastError()));
    }

    // 打开默认桌面（Default桌面）
    _defaultDesktop = OpenDesktop(TEXT("Default"), 0, FALSE, DESKTOP_ALL_ACCESS);
    if (_defaultDesktop == NULL) {
        throw std::runtime_error("Failed to open default desktop: " + std::to_string(GetLastError()));
    }

    // 切换到默认桌面
    if (!SetThreadDesktop(_defaultDesktop)) {
        CloseDesktop(_defaultDesktop);
        _defaultDesktop = NULL;
        throw std::runtime_error("Failed to set thread desktop to default: " + std::to_string(GetLastError()));
    }

    // 清理winlogon桌面句柄
    if (_winLogonDesktop != NULL) {
        CloseDesktop(_winLogonDesktop);
        _winLogonDesktop = NULL;
    }

    _isOnWinLogonDesktop = false;
}

void WinLogon::RestoreOriginalDesktop() {
    // 如果有默认桌面句柄，先关闭它
    if (_defaultDesktop != NULL) {
        CloseDesktop(_defaultDesktop);
        _defaultDesktop = NULL;
    }

    if (_oldThreadDesktop != NULL) {
        SetThreadDesktop(_oldThreadDesktop);
    }

    if (_winLogonDesktop != NULL) {
        CloseDesktop(_winLogonDesktop);
        _winLogonDesktop = NULL;
    }

    if (_oldProcessWinStation != NULL) {
        SetProcessWindowStation(_oldProcessWinStation);
    }

    if (_winSta0Station != NULL) {
        CloseWindowStation(_winSta0Station);
        _winSta0Station = NULL;
    }

    _isOnWinLogonDesktop = false;
}

bool WinLogon::AttachedToWinLogonDesktop() const {
    return _isOnWinLogonDesktop && (_winLogonDesktop != NULL);
}

void WinLogon::Dispose() {
    Dispose(true);
}

void WinLogon::Dispose(bool disposing) {
    if (!_disposed) {
        if (disposing) {
            RestoreOriginalDesktop();
        }
        _disposed = true;
    }
}

