#pragma once
#include <windows.h>
#include <interception/interception.h>
#include <memory>
#include <vector>
#include <unordered_map>
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

    // VK到扫描码的映射表
    std::unordered_map<WORD, WORD> vkToScanCode;

    // 修饰键状态跟踪
    struct ModifierState {
        bool ctrl = false;
        bool alt = false;
        bool shift = false;
        bool win = false;
    } modifierState;

public:
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
    bool KeyDown(BYTE vkCode, BYTE modifiers = 0);
    bool KeyUp(BYTE vkCode, BYTE modifiers = 0);
    bool SendKeyCombo(BYTE vkCode, BYTE modifiers);

    // VK码转扫描码
    WORD VkToScanCode(WORD vkCode);

    // 检查扩展键
    bool IsExtendedKey(WORD scanCode);

    // 强制停止
    void ForceStop();

private:
    void InitializeVkToScanCodeMap();
};