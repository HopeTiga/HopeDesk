#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace hope {
    namespace rtc {

        class WinLogon {
        public:
            WinLogon();
            ~WinLogon();

            // 获取当前窗口站下所有普通桌面的名称列表
            std::vector<std::wstring> GetNormalDesktops() const;

            // 切换到指定名称的普通桌面
            bool SwitchToDesktop(const std::wstring& desktopName);

            // 循环切换到下一个普通桌面
            void SwitchToNextNormalDesktop();

            bool AttachedToWinLogonDesktop() const;
            void SwitchToWinLogonDesktop();
            void RestoreOriginalDesktop();
            void SwitchToDefaultDesktop();
            void Dispose();
            void Dispose(bool disposing);
            static bool IsCurrentlyOnSecureDesktop();

        private:
            HDESK oldThreadDesktop = NULL;
            HWINSTA oldProcessWinStation = NULL;
            HWINSTA winSta0Station = NULL;
            HDESK winLogonDesktop = NULL;
            HDESK defaultDesktop = NULL;
            bool disposed = false;
            bool isOnWinLogonDesktop = false;

            // 静态回调函数，用于 EnumDesktops
            static BOOL CALLBACK EnumDesktopProc(LPTSTR desktopName, LPARAM lParam);
        };

    }
}