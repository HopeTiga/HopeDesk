#include "KeyMouseSimulator.h"
#include <tlhelp32.h>

namespace hope {
    namespace rtc {
        KeyMouseSimulator::KeyMouseSimulator()
            : interceptionContext(nullptr),
            interceptionKeyboard(0),
            interceptionMouse(0),
            isInitialized(false),
            isDestroying(false),
            cacheInitialized(false) {

            try {
                LOG_INFO("KeyMouseSimulator constructor started");

                // 初始化缓存数组
                memset(scanCodeCache, 0, sizeof(scanCodeCache));
                InitializeCache();

                LOG_INFO("KeyMouseSimulator constructor completed");
            }
            catch (...) {
                // logger 不再需要设置为 nullptr
            }
        }

        KeyMouseSimulator::~KeyMouseSimulator() {
            isDestroying = true;
            if (interceptionContext) {
                LOG_INFO("Destroying Interception context...");
                try {
                    interception_destroy_context(interceptionContext);
                    interceptionContext = nullptr;
                    LOG_INFO("Context destroyed successfully");
                }
                catch (...) {
                    LOG_ERROR("Exception during context destruction");
                    interceptionContext = nullptr;
                }
            }
        }

        bool KeyMouseSimulator::Initialize() {
            LOG_INFO("Initialize() called - entering function");
            LOG_INFO("Creating Interception context...");

            LOG_INFO("About to call interception_create_context()...");
            interceptionContext = interception_create_context();
            LOG_INFO("interception_create_context() returned");

            if (!interceptionContext) {
                LOG_ERROR("Failed to create Interception context");
                LOG_ERROR("Please ensure:");
                LOG_ERROR("1. Running with administrator privileges");
                LOG_ERROR("2. Interception driver is installed");
                LOG_ERROR("3. Driver service is running");
                return false;
            }

            LOG_INFO("Context created successfully");

            interceptionKeyboard = INTERCEPTION_KEYBOARD(0);
            interceptionMouse = INTERCEPTION_MOUSE(0);

            LOG_INFO("Device ID - Keyboard: %d, Mouse: %d", interceptionKeyboard, interceptionMouse);

            isInitialized = true;
            LOG_INFO("Initialization completed successfully");
            return true;
        }

        bool KeyMouseSimulator::SendKey(WORD scanCode, bool down, bool extended) {
            if (!interceptionContext || isDestroying) {
                LOG_ERROR("Invalid context or destroying");
                return false;
            }

            try {
                InterceptionKeyStroke keystroke = { 0 };
                keystroke.code = scanCode;
                keystroke.state = down ? INTERCEPTION_KEY_DOWN : INTERCEPTION_KEY_UP;

                if (extended) {
                    keystroke.state |= INTERCEPTION_KEY_E0;
                }

                keystroke.information = 0;

                // 直接转换，不需要 memcpy
                int result = interception_send(interceptionContext, interceptionKeyboard,
                    reinterpret_cast<InterceptionStroke*>(&keystroke), 1);

                if (result != 1) {
                    LOG_ERROR("Failed to send keyboard event, return value: %d", result);
                    return false;
                }


                return true;
            }
            catch (...) {
                LOG_ERROR("Exception occurred while sending keyboard event");
                return false;
            }
        }

        bool KeyMouseSimulator::MouseMove(int x, int y, bool absolute) {
            if (!interceptionContext || isDestroying) {
                LOG_ERROR("Invalid context or destroying");
                return false;
            }

            try {
                InterceptionMouseStroke mousestroke = { 0 };

                if (absolute) {
                    mousestroke.flags = INTERCEPTION_MOUSE_MOVE_ABSOLUTE;
                    thread_local static const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                    thread_local static const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

                    // 使用位运算：(x << 16) / screenWidth
                    mousestroke.x = (x << 16) / screenWidth;
                    mousestroke.y = (y << 16) / screenHeight;
                }
                else {
                    mousestroke.flags = INTERCEPTION_MOUSE_MOVE_RELATIVE;
                    mousestroke.x = x;
                    mousestroke.y = y;
                }

                mousestroke.state = 0;
                mousestroke.rolling = 0;
                mousestroke.information = 0;

                // 直接转换，不需要 memcpy
                int result = interception_send(interceptionContext, interceptionMouse,
                    reinterpret_cast<InterceptionStroke*>(&mousestroke), 1);

                if (result != 1) {
                    LOG_ERROR("Failed to send mouse movement, return value: %d", result);
                    return false;
                }

                return true;
            }
            catch (...) {
                LOG_ERROR("Exception during mouse movement");
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

            // 直接转换，不需要 memcpy
            return interception_send(interceptionContext, interceptionMouse,
                reinterpret_cast<InterceptionStroke*>(&mousestroke), 1) == 1;
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

            // 直接转换，不需要 memcpy
            return interception_send(interceptionContext, interceptionMouse,
                reinterpret_cast<InterceptionStroke*>(&mousestroke), 1) == 1;
        }

        bool KeyMouseSimulator::MouseWheel(int wheelDelta) {
            if (!interceptionContext || isDestroying) {
                return false;
            }

            InterceptionMouseStroke mousestroke = { 0 };
            mousestroke.state = INTERCEPTION_MOUSE_WHEEL;
            mousestroke.rolling = static_cast<short>(wheelDelta);

            // 直接转换，不需要 memcpy
            return interception_send(interceptionContext, interceptionMouse,
                reinterpret_cast<InterceptionStroke*>(&mousestroke), 1) == 1;
        }

        bool KeyMouseSimulator::IsNumLockOn() {
            return (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
        }

        // 初始化扫描码缓存
        void KeyMouseSimulator::InitializeCache() {
            if (cacheInitialized) return;

            for (int vk = 0; vk < 256; ++vk) {
                scanCodeCache[vk] = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
            }

            cacheInitialized = true;
        }

        // 性能优化：直接数组索引获取扫描码，O(1)时间复杂度
        WORD KeyMouseSimulator::GetCachedScanCode(DWORD vkCode) {
            if (vkCode >= 256) return 0;
            return scanCodeCache[vkCode];
        }

        bool KeyMouseSimulator::KeyDown(DWORD vkCode, BYTE modifiers) {
            if (vkCode > 0xFE) {
                LOG_ERROR("无效的VK码: %d", vkCode);
                return false;
            }

            // 处理主键盘的 "/" 键
            if (vkCode == 0xBF) {
                WORD scanCode = GetCachedScanCode(vkCode);
                return SendKey(scanCode, true, false);
            }

            // 统一处理所有按键
            WORD scanCode = GetCachedScanCode(vkCode);
            if (scanCode == 0) {
                LOG_ERROR("MapVirtualKey失败，VK: %d", vkCode);
                return false;
            }

            bool isExtended = IsExtendedKey(scanCode);
            return SendKey(scanCode, true, isExtended);
        }

        bool KeyMouseSimulator::KeyUp(DWORD vkCode, BYTE modifiers) {
            if (vkCode > 0xFE) {
                LOG_ERROR("无效的VK码: %d", vkCode);
                return false;
            }

            // 处理主键盘的 "/" 键
            if (vkCode == 0xBF) {
                WORD scanCode = GetCachedScanCode(vkCode);
                return SendKey(scanCode, false, false);
            }

            // 统一处理所有按键
            WORD scanCode = GetCachedScanCode(vkCode);
            if (scanCode == 0) {
                LOG_ERROR("MapVirtualKey失败，VK: %d", vkCode);
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
            LOG_INFO("Force stopping simulator...");
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
    }
}