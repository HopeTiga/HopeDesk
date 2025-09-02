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

    interceptionKeyboard = INTERCEPTION_KEYBOARD(0);
    interceptionMouse = INTERCEPTION_MOUSE(0);

    logger->info("Device ID - Keyboard: " + std::to_string(interceptionKeyboard) +
        ", Mouse: " + std::to_string(interceptionMouse));

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

        if (down) {
            keystroke.state = INTERCEPTION_KEY_DOWN;
        }
        else {
            keystroke.state = INTERCEPTION_KEY_UP;
        }

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
            mousestroke.flags = INTERCEPTION_MOUSE_MOVE_ABSOLUTE;
            mousestroke.x = (x * 65535) / GetSystemMetrics(SM_CXSCREEN);
            mousestroke.y = (y * 65535) / GetSystemMetrics(SM_CYSCREEN);
        }
        else {
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

    if (x >= 0 && y >= 0) {
        MouseMove(x, y, true);
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

bool KeyMouseSimulator::IsNumLockOn() {
    return (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
}

bool KeyMouseSimulator::KeyDown(DWORD vkCode, BYTE modifiers) {
    if (vkCode > 0xFE) {
        logger->error("无效的VK码: " + std::to_string(vkCode));
        return false;
    }

    // 检查是否是数字键盘按键
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9) {
        bool numLockOn = IsNumLockOn();
        logger->debug("NumLock状态: " + std::string(numLockOn ? "开启" : "关闭"));

        if (numLockOn) {
            // NumLock开启：发送数字，使用非扩展键
            WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
            if (scanCode == 0) {
                logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
                return false;
            }

            logger->debug("发送数字键盘数字: " + std::to_string(vkCode - VK_NUMPAD0) +
                ", 扫描码: " + std::to_string(scanCode) + ", 扩展键: false");
            return SendKey(scanCode, true, false);  // 强制非扩展键
        }
        else {
            // NumLock关闭：发送导航键，使用扩展键
            WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
            if (scanCode == 0) {
                logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
                return false;
            }

            logger->debug("发送数字键盘导航键: VK" + std::to_string(vkCode) +
                ", 扫描码: " + std::to_string(scanCode) + ", 扩展键: true");
            return SendKey(scanCode, true, true);   // 强制扩展键
        }
    }

    // 处理其他特殊数字键盘按键
    if (vkCode == VK_DECIMAL || vkCode == VK_SUBTRACT || vkCode == VK_ADD ||
        vkCode == VK_MULTIPLY || vkCode == VK_DIVIDE) {
        WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        if (scanCode == 0) {
            logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
            return false;
        }

        // 这些键通常不受NumLock影响，根据实际需要调整
        bool isExtended = (vkCode == VK_DIVIDE) ? true : false;  // 除号是扩展键
        logger->debug("发送数字键盘特殊键: VK" + std::to_string(vkCode) +
            ", 扫描码: " + std::to_string(scanCode) +
            ", 扩展键: " + (isExtended ? "true" : "false"));
        return SendKey(scanCode, true, isExtended);
    }

    if (vkCode == 0xBF) {  // 主键盘的 "/" 键
        WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        // 强制不使用扩展键标志，因为主键盘的 "/" 不是扩展键
        return SendKey(scanCode, true, false);  // 注意这里是 false
    }

    // 处理普通按键
    WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode == 0) {
        logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
        return false;
    }

    bool isExtended = IsExtendedKey(scanCode);
    return SendKey(scanCode, true, isExtended);
}

bool KeyMouseSimulator::KeyUp(DWORD vkCode, BYTE modifiers) {
    if (vkCode > 0xFE) {
        logger->error("无效的VK码: " + std::to_string(vkCode));
        return false;
    }

    // 检查是否是数字键盘按键
    if (vkCode >= VK_NUMPAD0 && vkCode <= VK_NUMPAD9) {
        bool numLockOn = IsNumLockOn();

        WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        if (scanCode == 0) {
            logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
            return false;
        }

        if (numLockOn) {
            return SendKey(scanCode, false, false);  // 数字，非扩展键
        }
        else {
            return SendKey(scanCode, false, true);   // 导航键，扩展键
        }
    }

    // 处理其他特殊数字键盘按键
    if (vkCode == VK_DECIMAL || vkCode == VK_SUBTRACT || vkCode == VK_ADD ||
        vkCode == VK_MULTIPLY || vkCode == VK_DIVIDE) {
        WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        if (scanCode == 0) {
            logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
            return false;
        }

        bool isExtended = (vkCode == VK_DIVIDE) ? true : false;
        return SendKey(scanCode, false, isExtended);
    }

    if (vkCode == 0xBF) {  // 主键盘的 "/" 键
        WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        // 强制不使用扩展键标志，因为主键盘的 "/" 不是扩展键
        return SendKey(scanCode, false, false);  // 注意这里是 false
    }

    // 处理普通按键
    WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    if (scanCode == 0) {
        logger->error("MapVirtualKey失败，VK: " + std::to_string(vkCode));
        return false;
    }

    bool isExtended = IsExtendedKey(scanCode);
    return SendKey(scanCode, false, isExtended);
}

WORD KeyMouseSimulator::VkToScanCode(DWORD vkCode) {
    // Try extended mapping first, then regular mapping
    WORD scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC_EX);
    if (scanCode == 0) {
        scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    }
    return scanCode;
}

bool KeyMouseSimulator::SendKeyCombo(BYTE vkCode, BYTE modifiers) {
    // Press modifiers first
    if (modifiers & 0x02) { // Ctrl
        SendKey(0x1D, true);
    }
    if (modifiers & 0x04) { // Alt  
        SendKey(0x38, true);
    }
    if (modifiers & 0x01) { // Shift
        SendKey(0x2A, true);
    }
    if (modifiers & 0x08) { // Windows
        SendKey(0x5B, true, true);
    }

    // Press main key
    WORD scanCode = VkToScanCode(vkCode);
    bool isExtended = IsExtendedKey(scanCode);
    SendKey(scanCode, true, isExtended);

    // Release main key
    SendKey(scanCode, false, isExtended);

    // Release modifiers in reverse order
    if (modifiers & 0x08) { // Windows
        SendKey(0x5B, false, true);
    }
    if (modifiers & 0x01) { // Shift
        SendKey(0x2A, false);
    }
    if (modifiers & 0x04) { // Alt
        SendKey(0x38, false);
    }
    if (modifiers & 0x02) { // Ctrl
        SendKey(0x1D, false);
    }

    return true;
}

void KeyMouseSimulator::ForceStop() {
    isDestroying = true;
    logger->info("Force stopping simulator...");
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