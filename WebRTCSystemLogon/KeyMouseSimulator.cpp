#include "KeyMouseSimulator.h"
#include <tlhelp32.h>

KeyMouseSimulator::KeyMouseSimulator()
    : interceptionContext(nullptr),
    interceptionKeyboard(0),
    interceptionMouse(0),
    isInitialized(false),
    isDestroying(false) {

    try {
        logger = Logger::getInstance();
        logger->info("KeyMouseSimulator constructor started");
        InitializeVkToScanCodeMap();
        logger->info("KeyMouseSimulator constructor completed");
    }
    catch (...) {
        logger = nullptr;
    }
}

KeyMouseSimulator::~KeyMouseSimulator() {
    isDestroying = true;
    if (interceptionContext) {
        logger->info("Destroying Interception context...");
        try {
            interception_destroy_context(interceptionContext);
            interceptionContext = nullptr;
            logger->info("Context destroyed successfully");
        }
        catch (...) {
            logger->error("Exception during context destruction");
            interceptionContext = nullptr;
        }
    }
}

bool KeyMouseSimulator::Initialize() {
    logger->info("Initialize() called - entering function");
    logger->info("Creating Interception context...");

    // 直接创建context，和服务代码一样简单
    logger->info("About to call interception_create_context()...");
    interceptionContext = interception_create_context();
    logger->info("interception_create_context() returned");

    if (!interceptionContext) {
        logger->error("Failed to create Interception context");
        logger->error("Please ensure:");
        logger->error("1. Running with administrator privileges");
        logger->error("2. Interception driver is installed");
        logger->error("3. Driver service is running");
        return false;
    }

    logger->info("Context created successfully");

    // 获取设备，和服务代码一样
    interceptionKeyboard = INTERCEPTION_KEYBOARD(0);
    interceptionMouse = INTERCEPTION_MOUSE(0);

    logger->info("Device ID - Keyboard: " + std::to_string(interceptionKeyboard) +
        ", Mouse: " + std::to_string(interceptionMouse));

    // 不需要检查设备是否有效，服务代码也没检查
    // 因为即使invalid，send函数也能工作

    isInitialized = true;
    logger->info("Initialization completed successfully");
    return true;
}

bool KeyMouseSimulator::SendKey(WORD scanCode, bool down, bool extended) {
    if (!interceptionContext || isDestroying) {
        logger->error("Invalid context or destroying");
        return false;
    }

    try {
        InterceptionKeyStroke keystroke = { 0 };
        keystroke.code = scanCode;

        // 设置按键状态
        if (down) {
            keystroke.state = INTERCEPTION_KEY_DOWN;
        }
        else {
            keystroke.state = INTERCEPTION_KEY_UP;
        }

        // 如果是扩展键，添加E0标志
        if (extended) {
            keystroke.state |= INTERCEPTION_KEY_E0;
        }

        keystroke.information = 0;

        InterceptionStroke stroke;
        memset(&stroke, 0, sizeof(stroke));
        memcpy(&stroke, &keystroke, sizeof(keystroke));

        int result = interception_send(interceptionContext, interceptionKeyboard, &stroke, 1);
        if (result != 1) {
            logger->error("Failed to send keyboard event, return value: " + std::to_string(result));
            return false;
        }

        logger->debug("Keyboard event sent - Scancode: " + std::to_string(scanCode) +
            ", State: " + (down ? "Down" : "Up") +
            ", Extended: " + (extended ? "Yes" : "No"));
        return true;
    }
    catch (...) {
        logger->error("Exception occurred while sending keyboard event");
        return false;
    }
}

bool KeyMouseSimulator::MouseMove(int x, int y, bool absolute) {
    if (!interceptionContext || isDestroying) {
        logger->error("Invalid context or destroying");
        return false;
    }

    try {
        InterceptionMouseStroke mousestroke = { 0 };
        mousestroke.state = 0;

        if (absolute) {
            // 重要！转换为归一化坐标 (0-65535)，就像SendInput一样
            mousestroke.flags = INTERCEPTION_MOUSE_MOVE_ABSOLUTE;
            mousestroke.x = (x * 65535) / GetSystemMetrics(SM_CXSCREEN);
            mousestroke.y = (y * 65535) / GetSystemMetrics(SM_CYSCREEN);
        }
        else {
            // 相对移动保持不变
            mousestroke.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
            mousestroke.x = x;
            mousestroke.y = y;
        }

        mousestroke.rolling = 0;
        mousestroke.information = 0;

        InterceptionStroke stroke;
        memset(&stroke, 0, sizeof(stroke));
        memcpy(&stroke, &mousestroke, sizeof(mousestroke));

        int result = interception_send(interceptionContext, interceptionMouse, &stroke, 1);
        if (result != 1) {
            logger->error("Failed to send mouse movement, return value: " + std::to_string(result));
            return false;
        }

        logger->debug("Mouse moved - Original: (" + std::to_string(x) + ", " + std::to_string(y) +
            "), Normalized: (" + std::to_string(mousestroke.x) + ", " + std::to_string(mousestroke.y) + ")");
        return true;
    }
    catch (...) {
        logger->error("Exception during mouse movement");
        return false;
    }
}

bool KeyMouseSimulator::MouseButtonDown(int buttonType, int x, int y) {
    if (!interceptionContext || isDestroying) {
        return false;
    }

    // 如果提供了坐标，先移动鼠标
    if (x >= 0 && y >= 0) {
        MouseMove(x, y, true);  // 这里会自动进行归一化
    }

    InterceptionMouseStroke mousestroke = { 0 };

    switch (buttonType) {
    case 0: mousestroke.state = INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN; break;
    case 1: mousestroke.state = INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN; break;
    case 2: mousestroke.state = INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN; break;
    default: return false;
    }

    InterceptionStroke stroke;
    memset(&stroke, 0, sizeof(stroke));
    memcpy(&stroke, &mousestroke, sizeof(mousestroke));

    return interception_send(interceptionContext, interceptionMouse, &stroke, 1) == 1;
}

bool KeyMouseSimulator::MouseButtonUp(int buttonType) {
    if (!interceptionContext || isDestroying) {
        return false;
    }

    InterceptionMouseStroke mousestroke = { 0 };

    switch (buttonType) {
    case 0: mousestroke.state = INTERCEPTION_MOUSE_LEFT_BUTTON_UP; break;
    case 1: mousestroke.state = INTERCEPTION_MOUSE_RIGHT_BUTTON_UP; break;
    case 2: mousestroke.state = INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP; break;
    default: return false;
    }

    InterceptionStroke stroke;
    memset(&stroke, 0, sizeof(stroke));
    memcpy(&stroke, &mousestroke, sizeof(mousestroke));

    return interception_send(interceptionContext, interceptionMouse, &stroke, 1) == 1;
}

bool KeyMouseSimulator::MouseWheel(int wheelDelta) {
    if (!interceptionContext || isDestroying) {
        return false;
    }

    InterceptionMouseStroke mousestroke = { 0 };
    mousestroke.state = INTERCEPTION_MOUSE_WHEEL;
    mousestroke.rolling = (short)(wheelDelta);

    InterceptionStroke stroke;
    memset(&stroke, 0, sizeof(stroke));
    memcpy(&stroke, &mousestroke, sizeof(mousestroke));

    return interception_send(interceptionContext, interceptionMouse, &stroke, 1) == 1;
}

bool KeyMouseSimulator::KeyDown(BYTE vkCode, BYTE modifiers) {
    // 按下修饰键
    if (modifiers & 0x02 && !modifierState.ctrl) { // Ctrl
        SendKey(0x1D, true);  // SCANCODE_CTRL_LEFT
        modifierState.ctrl = true;
    }
    if (modifiers & 0x04 && !modifierState.alt) { // Alt  
        SendKey(0x38, true);  // SCANCODE_ALT_LEFT
        modifierState.alt = true;
    }
    if (modifiers & 0x01 && !modifierState.shift) { // Shift
        SendKey(0x2A, true);  // SCANCODE_SHIFT_LEFT
        modifierState.shift = true;
    }
    if (modifiers & 0x08 && !modifierState.win) { // Windows
        SendKey(0x5B, true, true);  // SCANCODE_LWIN (extended)
        modifierState.win = true;
    }

    // 按下主键
    WORD scanCode = VkToScanCode(vkCode);
    bool isExtended = IsExtendedKey(scanCode);
    return SendKey(scanCode, true, isExtended);
}

bool KeyMouseSimulator::KeyUp(BYTE vkCode, BYTE modifiers) {
    // 释放主键
    WORD scanCode = VkToScanCode(vkCode);
    bool isExtended = IsExtendedKey(scanCode);
    SendKey(scanCode, false, isExtended);

    // 释放修饰键（逆序）
    if (modifiers & 0x08 && modifierState.win) { // Windows
        SendKey(0x5B, false, true);  // SCANCODE_LWIN (extended)
        modifierState.win = false;
    }
    if (modifiers & 0x01 && modifierState.shift) { // Shift
        SendKey(0x2A, false);  // SCANCODE_SHIFT_LEFT
        modifierState.shift = false;
    }
    if (modifiers & 0x04 && modifierState.alt) { // Alt
        SendKey(0x38, false);  // SCANCODE_ALT_LEFT
        modifierState.alt = false;
    }
    if (modifiers & 0x02 && modifierState.ctrl) { // Ctrl
        SendKey(0x1D, false);  // SCANCODE_CTRL_LEFT
        modifierState.ctrl = false;
    }

    return true;
}

bool KeyMouseSimulator::SendKeyCombo(BYTE vkCode, BYTE modifiers) {
    if (!KeyDown(vkCode, modifiers)) {
        return false;
    }

    Sleep(50);

    return KeyUp(vkCode, modifiers);
}

void KeyMouseSimulator::ForceStop() {
    isDestroying = true;
    logger->info("Force stopping simulator...");
}

WORD KeyMouseSimulator::VkToScanCode(WORD vkCode) {
    auto it = vkToScanCode.find(vkCode);
    if (it != vkToScanCode.end()) {
        return it->second;
    }

    // 如果映射表中没有，使用Windows API
    return MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
}

bool KeyMouseSimulator::IsExtendedKey(WORD scanCode) {
    switch (scanCode) {
    case 0x1C: // Enter (numpad)
    case 0x1D: // Right Ctrl  
    case 0x35: // Divide (numpad)
    case 0x38: // Right Alt
    case 0x47: // Home
    case 0x48: // Up
    case 0x49: // Page Up
    case 0x4B: // Left
    case 0x4D: // Right
    case 0x4F: // End
    case 0x50: // Down
    case 0x51: // Page Down
    case 0x52: // Insert
    case 0x53: // Delete
    case 0x5B: // Left Windows
    case 0x5C: // Right Windows
    case 0x5D: // Application
        return true;
    }
    return false;
}


void KeyMouseSimulator::InitializeVkToScanCodeMap() {
    // 基础按键映射
    vkToScanCode[VK_ESCAPE] = 0x01;
    vkToScanCode['1'] = 0x02;
    vkToScanCode['2'] = 0x03;
    vkToScanCode['3'] = 0x04;
    vkToScanCode['4'] = 0x05;
    vkToScanCode['5'] = 0x06;
    vkToScanCode['6'] = 0x07;
    vkToScanCode['7'] = 0x08;
    vkToScanCode['8'] = 0x09;
    vkToScanCode['9'] = 0x0A;
    vkToScanCode['0'] = 0x0B;
    vkToScanCode[VK_OEM_MINUS] = 0x0C;
    vkToScanCode[VK_OEM_PLUS] = 0x0D;
    vkToScanCode[VK_BACK] = 0x0E;
    vkToScanCode[VK_TAB] = 0x0F;

    // 字母键
    vkToScanCode['Q'] = 0x10;
    vkToScanCode['W'] = 0x11;
    vkToScanCode['E'] = 0x12;
    vkToScanCode['R'] = 0x13;
    vkToScanCode['T'] = 0x14;
    vkToScanCode['Y'] = 0x15;
    vkToScanCode['U'] = 0x16;
    vkToScanCode['I'] = 0x17;
    vkToScanCode['O'] = 0x18;
    vkToScanCode['P'] = 0x19;
    vkToScanCode[VK_OEM_4] = 0x1A;  // [
    vkToScanCode[VK_OEM_6] = 0x1B;  // ]
    vkToScanCode[VK_RETURN] = 0x1C;
    vkToScanCode[VK_CONTROL] = 0x1D;
    vkToScanCode[VK_LCONTROL] = 0x1D;

    vkToScanCode['A'] = 0x1E;
    vkToScanCode['S'] = 0x1F;
    vkToScanCode['D'] = 0x20;
    vkToScanCode['F'] = 0x21;
    vkToScanCode['G'] = 0x22;
    vkToScanCode['H'] = 0x23;
    vkToScanCode['J'] = 0x24;
    vkToScanCode['K'] = 0x25;
    vkToScanCode['L'] = 0x26;
    vkToScanCode[VK_OEM_1] = 0x27;  // ;
    vkToScanCode[VK_OEM_7] = 0x28;  // '
    vkToScanCode[VK_OEM_3] = 0x29;  // `
    vkToScanCode[VK_SHIFT] = 0x2A;
    vkToScanCode[VK_LSHIFT] = 0x2A;
    vkToScanCode[VK_OEM_5] = 0x2B;  // \
                
    vkToScanCode['Z'] = 0x2C;
    vkToScanCode['X'] = 0x2D;
    vkToScanCode['C'] = 0x2E;
    vkToScanCode['V'] = 0x2F;
    vkToScanCode['B'] = 0x30;
    vkToScanCode['N'] = 0x31;
    vkToScanCode['M'] = 0x32;
    vkToScanCode[VK_OEM_COMMA] = 0x33;
    vkToScanCode[VK_OEM_PERIOD] = 0x34;
    vkToScanCode[VK_OEM_2] = 0x35;  // /
    vkToScanCode[VK_RSHIFT] = 0x36;
    vkToScanCode[VK_MULTIPLY] = 0x37;
    vkToScanCode[VK_MENU] = 0x38;
    vkToScanCode[VK_LMENU] = 0x38;
    vkToScanCode[VK_SPACE] = 0x39;
    vkToScanCode[VK_CAPITAL] = 0x3A;

    // F键
    vkToScanCode[VK_F1] = 0x3B;
    vkToScanCode[VK_F2] = 0x3C;
    vkToScanCode[VK_F3] = 0x3D;
    vkToScanCode[VK_F4] = 0x3E;
    vkToScanCode[VK_F5] = 0x3F;
    vkToScanCode[VK_F6] = 0x40;
    vkToScanCode[VK_F7] = 0x41;
    vkToScanCode[VK_F8] = 0x42;
    vkToScanCode[VK_F9] = 0x43;
    vkToScanCode[VK_F10] = 0x44;
    vkToScanCode[VK_F11] = 0x57;
    vkToScanCode[VK_F12] = 0x58;

    // 扩展键（需要E0前缀）
    vkToScanCode[VK_DELETE] = 0x53;
    vkToScanCode[VK_LEFT] = 0x4B;
    vkToScanCode[VK_RIGHT] = 0x4D;
    vkToScanCode[VK_UP] = 0x48;
    vkToScanCode[VK_DOWN] = 0x50;
    vkToScanCode[VK_INSERT] = 0x52;
    vkToScanCode[VK_HOME] = 0x47;
    vkToScanCode[VK_END] = 0x4F;
    vkToScanCode[VK_PRIOR] = 0x49;  // Page Up
    vkToScanCode[VK_NEXT] = 0x51;   // Page Down
    vkToScanCode[VK_LWIN] = 0x5B;
    vkToScanCode[VK_RWIN] = 0x5C;
    vkToScanCode[VK_APPS] = 0x5D;
}
