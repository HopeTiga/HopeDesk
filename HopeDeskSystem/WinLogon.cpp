#include "WinLogon.h"
#include <stdexcept>
#include <string>
#include <tchar.h>
#include <algorithm>   // for std::find

#define WINSTA_ALL_ACCESS 0x37F
#define DESKTOP_ALL_ACCESS 0x01FF

namespace hope {
    namespace rtc {

        // 静态回调：枚举桌面并过滤掉 winlogon 和 ScreenSaver
        BOOL CALLBACK WinLogon::EnumDesktopProc(LPTSTR desktopName, LPARAM lParam) {
            auto* vec = reinterpret_cast<std::vector<std::wstring>*>(lParam);
            if (_tcsicmp(desktopName, TEXT("winlogon")) != 0 &&
                _tcsicmp(desktopName, TEXT("ScreenSaver")) != 0) {
                vec->push_back(desktopName);
            }
            return TRUE;
        }

        WinLogon::WinLogon() {
            SwitchToWinLogonDesktop();
        }

        WinLogon::~WinLogon() {
            Dispose(false);
        }

        void WinLogon::SwitchToWinLogonDesktop() {
            if (isOnWinLogonDesktop) {
                return;
            }

            winSta0Station = OpenWindowStation(TEXT("WinSta0"), FALSE, WINSTA_ALL_ACCESS);
            if (winSta0Station == NULL) {
                throw std::runtime_error("Failed to open WinSta0 window station: " + std::to_string(GetLastError()));
            }

            oldProcessWinStation = GetProcessWindowStation();

            if (!SetProcessWindowStation(winSta0Station)) {
                CloseWindowStation(winSta0Station);
                throw std::runtime_error("Failed to set process window station: " + std::to_string(GetLastError()));
            }

            winLogonDesktop = OpenDesktop(TEXT("winlogon"), 0, FALSE, DESKTOP_ALL_ACCESS);
            if (winLogonDesktop == NULL) {
                SetProcessWindowStation(oldProcessWinStation);
                CloseWindowStation(winSta0Station);
                throw std::runtime_error("Failed to open winlogon desktop: " + std::to_string(GetLastError()));
            }

            oldThreadDesktop = GetThreadDesktop(GetCurrentThreadId());

            if (!SetThreadDesktop(winLogonDesktop)) {
                CloseDesktop(winLogonDesktop);
                SetProcessWindowStation(oldProcessWinStation);
                CloseWindowStation(winSta0Station);
                throw std::runtime_error("Failed to set thread desktop: " + std::to_string(GetLastError()));
            }

            isOnWinLogonDesktop = true;
        }

        bool WinLogon::IsCurrentlyOnSecureDesktop() {
            HDESK currentDesktop = GetThreadDesktop(GetCurrentThreadId());
            if (currentDesktop == NULL) {
                return false;
            }

            TCHAR desktopName[256] = { 0 };
            DWORD needed = 0;
            if (!GetUserObjectInformation(currentDesktop, UOI_NAME, desktopName, sizeof(desktopName), &needed)) {
                return false;
            }

            return (_tcsicmp(desktopName, TEXT("winlogon")) == 0);
        }

        void WinLogon::SwitchToDefaultDesktop() {
            if (!isOnWinLogonDesktop) {
                return;
            }

            if (winSta0Station == NULL) {
                winSta0Station = OpenWindowStation(TEXT("WinSta0"), FALSE, WINSTA_ALL_ACCESS);
                if (winSta0Station == NULL) {
                    throw std::runtime_error("Failed to open WinSta0 window station: " + std::to_string(GetLastError()));
                }
            }

            if (!SetProcessWindowStation(winSta0Station)) {
                throw std::runtime_error("Failed to set process window station: " + std::to_string(GetLastError()));
            }

            defaultDesktop = OpenDesktop(TEXT("Default"), 0, FALSE, DESKTOP_ALL_ACCESS);
            if (defaultDesktop == NULL) {
                throw std::runtime_error("Failed to open default desktop: " + std::to_string(GetLastError()));
            }

            if (!SetThreadDesktop(defaultDesktop)) {
                CloseDesktop(defaultDesktop);
                defaultDesktop = NULL;
                throw std::runtime_error("Failed to set thread desktop to default: " + std::to_string(GetLastError()));
            }

            if (winLogonDesktop != NULL) {
                CloseDesktop(winLogonDesktop);
                winLogonDesktop = NULL;
            }

            isOnWinLogonDesktop = false;
        }

        void WinLogon::RestoreOriginalDesktop() {
            if (defaultDesktop != NULL) {
                CloseDesktop(defaultDesktop);
                defaultDesktop = NULL;
            }

            if (oldThreadDesktop != NULL) {
                SetThreadDesktop(oldThreadDesktop);
            }

            if (winLogonDesktop != NULL) {
                CloseDesktop(winLogonDesktop);
                winLogonDesktop = NULL;
            }

            if (oldProcessWinStation != NULL) {
                SetProcessWindowStation(oldProcessWinStation);
            }

            if (winSta0Station != NULL) {
                CloseWindowStation(winSta0Station);
                winSta0Station = NULL;
            }

            isOnWinLogonDesktop = false;
        }

        bool WinLogon::AttachedToWinLogonDesktop() const {
            return isOnWinLogonDesktop && (winLogonDesktop != NULL);
        }

        void WinLogon::Dispose() {
            Dispose(true);
        }

        void WinLogon::Dispose(bool disposing) {
            if (!disposed) {
                if (disposing) {
                    RestoreOriginalDesktop();
                }
                disposed = true;
            }
        }

        // 获取普通桌面列表（const 成员函数）
        std::vector<std::wstring> WinLogon::GetNormalDesktops() const {
            std::vector<std::wstring> desktops;
            HWINSTA hwinsta = GetProcessWindowStation();
            if (hwinsta == NULL) {
                return desktops;
            }
            // 使用静态回调函数，传递 vector 指针
            EnumDesktops(hwinsta, EnumDesktopProc, reinterpret_cast<LPARAM>(&desktops));
            return desktops;
        }

        bool WinLogon::SwitchToDesktop(const std::wstring& desktopName) {
            HWINSTA hwinsta = GetProcessWindowStation();
            if (hwinsta == NULL) {
                // 尝试切换到 WinSta0
                HWINSTA winsta0 = OpenWindowStation(TEXT("WinSta0"), FALSE, WINSTA_ALL_ACCESS);
                if (winsta0 == NULL) return false;
                if (!SetProcessWindowStation(winsta0)) {
                    CloseWindowStation(winsta0);
                    return false;
                }
                CloseWindowStation(winsta0); // 临时使用后可关闭
            }

            HDESK hDesktop = OpenDesktop(desktopName.c_str(), 0, FALSE, DESKTOP_ALL_ACCESS);
            if (hDesktop == NULL) {
                return false;
            }
            if (!SetThreadDesktop(hDesktop)) {
                CloseDesktop(hDesktop);
                return false;
            }
            CloseDesktop(hDesktop);  // SetThreadDesktop 已增加引用计数
            return true;
        }

        void WinLogon::SwitchToNextNormalDesktop() {
            auto desktops = GetNormalDesktops();
            if (desktops.empty()) return;

            // 获取当前桌面名称
            HDESK currentDesktop = GetThreadDesktop(GetCurrentThreadId());
            TCHAR currentName[256] = { 0 };
            DWORD needed = 0;
            std::wstring currentDesktopName;
            if (currentDesktop != NULL && GetUserObjectInformation(currentDesktop, UOI_NAME, currentName, sizeof(currentName), &needed)) {
                currentDesktopName = currentName;
            }

            auto it = std::find(desktops.begin(), desktops.end(), currentDesktopName);
            size_t nextIndex = 0;
            if (it != desktops.end()) {
                nextIndex = (it - desktops.begin() + 1) % desktops.size();
            }
            SwitchToDesktop(desktops[nextIndex]);
        }

    }
}