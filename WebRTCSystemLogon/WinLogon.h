#pragma once
#include <Windows.h>
#include <string>

class WinLogon {
private:
    HDESK _oldThreadDesktop = NULL;
    HWINSTA _oldProcessWinStation = NULL;
    HWINSTA _winSta0Station = NULL;
    HDESK _winLogonDesktop = NULL;
    HDESK _defaultDesktop = NULL;  // 添加默认桌面句柄
    bool _disposed = false;
    bool _isOnWinLogonDesktop = false;  // 标记当前是否在安全桌面

public:
    WinLogon();
    ~WinLogon();

    bool AttachedToWinLogonDesktop() const;
    void SwitchToWinLogonDesktop();
    void RestoreOriginalDesktop();
    void SwitchToDefaultDesktop();  // 新增：切换回普通桌面
    void Dispose();
    void Dispose(bool disposing);
    static bool IsCurrentlyOnSecureDesktop();
};