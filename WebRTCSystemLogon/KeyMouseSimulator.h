#pragma once
#include <windows.h>
#include <interception/interception.h>
#include <memory>
#include <vector>
#include <atomic>
#include "Logger.h"

class KeyMouseSimulator {
private:
    InterceptionContext interceptionContext;
    InterceptionDevice interceptionKeyboard;
    InterceptionDevice interceptionMouse;
    Logger* logger;
    std::atomic<bool> isInitialized;
    std::atomic<bool> isDestroying;

public:

    bool IsNumLockOn();

    KeyMouseSimulator();
    ~KeyMouseSimulator();

    // 初始化
    bool Initialize();

    // 鼠标操作
    bool MouseMove(int x, int y, bool absolute = true);
    bool MouseButtonDown(int buttonType, int x = -1, int y = -1);
    bool MouseButtonUp(int buttonType);
    bool MouseWheel(int wheelDelta);

    // 键盘操作 - 直接使用扫描码
    bool SendKey(WORD scanCode, bool down = true, bool extended = false);
    bool KeyDown(DWORD vkCode, BYTE modifiers = 0);
    bool KeyUp(DWORD vkCode, BYTE modifiers = 0);
    bool SendKeyCombo(BYTE vkCode, BYTE modifiers);

    // VK码转扫描码 - 直接用Windows API
    WORD VkToScanCode(DWORD vkCode);
    // 检查扩展键
    bool IsExtendedKey(WORD scanCode);

    // 强制停止
    void ForceStop();
};