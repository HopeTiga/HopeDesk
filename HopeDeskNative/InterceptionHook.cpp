#include "InterceptionHook.h"
#include "VideoWidget.h"
#include "WebrtcManager.h"
#include "Utils.h"
#include <QWidget>
#include <QApplication>

namespace hope{

    namespace rtc{

InterceptionHook::InterceptionHook(QObject* parent)
    : QObject(parent)
    , context(nullptr)
    , keyboard(0)
    , mouse(0)
    , targetWidget(nullptr)
    , targetHwnd(nullptr)
    , webrtcManager(nullptr)
    , running(false)
    , initialized(false)
    , lastMouseX(0)
    , lastMouseY(0)
    , numLockState(false)
{
    LOG_INFO("InterceptionHook constructor");

    // Get screen dimensions
    screenWidth = GetSystemMetrics(SM_CXSCREEN);
    screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

InterceptionHook::~InterceptionHook()
{
    stopCapture();
    LOG_INFO("InterceptionHook destroyed");
}

void InterceptionHook::setTargetWidget(VideoWidget* widget)
{
    targetWidget = widget;
    if (widget) {
        targetHwnd = reinterpret_cast<HWND>(widget->winId());
        LOG_INFO("Target widget set, HWND: %p", targetHwnd);
    }
}

void InterceptionHook::setWebrtcManager(std::shared_ptr<WebrtcManager> webrtcManager)
{
    this->webrtcManager = webrtcManager;
    LOG_INFO("Remote client set");
}

void InterceptionHook::setVideoSize(int width, int height)
{

}

bool InterceptionHook::startCapture()
{
    if (running) {
        LOG_WARN("Capture already running");
        return true;
    }

    LOG_INFO("Starting Interception capture...");

    // Create Interception context
    context = interception_create_context();
    if (!context) {
        LOG_ERROR("Failed to create Interception context");
        LOG_ERROR("Please ensure:");
        LOG_ERROR("1. Running with administrator privileges");
        LOG_ERROR("2. Interception driver is installed");
        LOG_ERROR("3. Driver service is running");
        return false;
    }

    LOG_INFO("Interception context created successfully");

    // Set device IDs
    keyboard = INTERCEPTION_KEYBOARD(0);
    mouse = INTERCEPTION_MOUSE(0);

    // Set filters: capture all keyboard and mouse devices
    interception_set_filter(context, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);
    interception_set_filter(context, interception_is_mouse, INTERCEPTION_FILTER_MOUSE_ALL);

    initialized = true;
    running = true;

    // Start capture thread
    captureThread = std::thread(&InterceptionHook::captureThreadFunc, this);

    LOG_INFO("Capture thread started");
    return true;
}

void InterceptionHook::stopCapture()
{
    if (!running) {
        return;
    }

    LOG_INFO("Stopping capture...");
    running = false;

    // Wait for thread to finish
    if (captureThread.joinable()) {
        captureThread.join();
    }


    // Destroy context
    if (context) {
        interception_destroy_context(context);
        context = nullptr;
    }

    initialized = false;
    LOG_INFO("Capture stopped");
}

bool InterceptionHook::isInTargetWindow() const
{
    if (!targetHwnd) {
        return false;
    }

    // Get mouse position
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    // Get window under cursor
    HWND hwndUnderCursor = WindowFromPoint(cursorPos);

    // Check if it's target window or its child
    return (hwndUnderCursor == targetHwnd || IsChild(targetHwnd, hwndUnderCursor));
}

void InterceptionHook::captureThreadFunc()
{
    LOG_INFO("Capture thread started");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    InterceptionDevice device;
    InterceptionStroke stroke;

    while (running) {
        device = interception_wait_with_timeout(context,3000);
        if (!running) break;
        if (device == 0) continue;
        if (interception_receive(context, device, &stroke, 1) <= 0) continue;

        if (interception_is_keyboard(device)) {
            InterceptionKeyStroke* keystroke = reinterpret_cast<InterceptionKeyStroke*>(&stroke);

            // Global NumLock key monitoring
            bool isPress = !(keystroke->state & INTERCEPTION_KEY_UP);
            if (keystroke->code == 0x45 && isPress) {
                numLockState = !numLockState.load();
                LOG_INFO("NumLock toggled to: %s", numLockState ? "ON" : "OFF");
            }

            HWND foregroundWnd = GetForegroundWindow();
            if (foregroundWnd == targetHwnd || IsChild(targetHwnd, foregroundWnd)) {
                processKeyboardEvent(*keystroke);
                continue;
            }
        }
        else if (interception_is_mouse(device)) {
            InterceptionMouseStroke* mousestroke = reinterpret_cast<InterceptionMouseStroke*>(&stroke);
            // 相对模式(游戏视角)：本地光标已隐藏且会漂移甚至离开窗口，
            // 不能再用 isInTargetWindow() 做门控，否则视角会卡死
            if (webrtcManager && webrtcManager->relativeMouseMode.load()) {
                processMouseEvent(*mousestroke);
            }
            else if (isInTargetWindow()) {
                processMouseEvent(*mousestroke);
            }
        }

        interception_send(context, device, &stroke, 1);
    }

    LOG_INFO("Capture thread exiting");
}

void InterceptionHook::processKeyboardEvent(InterceptionKeyStroke& keystroke)
{
    bool isPress = !(keystroke.state & INTERCEPTION_KEY_UP);

    if(keystroke.code==42 && (keystroke.state==2 || keystroke.state==3)) return;

    // Use system API for conversion (handles most keys correctly)
    DWORD vkCode = MapVirtualKey(keystroke.code, MAPVK_VSC_TO_VK_EX);


    // Handle special case: numpad keys need to be distinguished based on NumLock state
    if (!(keystroke.state & INTERCEPTION_KEY_E0) &&
        ((keystroke.code >= 0x47 && keystroke.code <= 0x53) || keystroke.code == 0x52)) {

        bool numLockOn = isNumLockOn();

        if (!numLockOn) {
            // NumLock OFF: Send main keyboard numeric characters
            switch (keystroke.code) {
            case 0x52: vkCode = 0x30; break;  // '0'
            case 0x4F: vkCode = 0x31; break;  // '1'
            case 0x50: vkCode = 0x32; break;  // '2'
            case 0x51: vkCode = 0x33; break;  // '3'
            case 0x4B: vkCode = 0x34; break;  // '4'
            case 0x4C: vkCode = 0x35; break;  // '5'
            case 0x4D: vkCode = 0x36; break;  // '6'
            case 0x47: vkCode = 0x37; break;  // '7'
            case 0x48: vkCode = 0x38; break;  // '8'
            case 0x49: vkCode = 0x39; break;  // '9'
            case 0x53: vkCode = VK_DECIMAL; break;
            }
        }
    }

    if(keystroke.code == 75 && (keystroke.state==2 || keystroke.state==3)) vkCode = VK_LEFT;

    if(keystroke.code == 77 && (keystroke.state==2 || keystroke.state==3)) vkCode = VK_RIGHT;

    // 跟踪 Ctrl/Alt 物理按下状态(拦截后 OS 看不到,不能用 GetAsyncKeyState)
    if (vkCode == VK_CONTROL || vkCode == VK_LCONTROL || vkCode == VK_RCONTROL) ctrlDown = isPress;
    if (vkCode == VK_MENU || vkCode == VK_LMENU || vkCode == VK_RMENU) altDown = isPress;

    // Ctrl+Alt+F:本地切换全屏,不转发对端(F 完全消费)
    if (vkCode == 'F' && targetWidget) {
        if (isPress && ctrlDown && altDown) {
            if (!fullscreenHotkeyConsumed) {
                fullscreenHotkeyConsumed = true;
                VideoWidget* w = targetWidget;
                QMetaObject::invokeMethod(w, [w]() {
                    if (w->isInFullScreenMode()) w->exitFullScreen();
                    else w->enterFullScreen();
                }, Qt::QueuedConnection);
            }
            return;  // 不转发 F
        }
        if (!isPress) fullscreenHotkeyConsumed = false;  // F 抬起,允许下次再切
    }

    char modifiers = getCurrentModifiers();
    sendKeyEvent(isPress, vkCode, modifiers);
}

void InterceptionHook::processMouseEvent(InterceptionMouseStroke& mousestroke)
{
    // 远程光标被游戏隐藏 -> 相对模式(转发硬件增量用于视角)
    // 否则 -> 绝对模式(所见即所得)
    const bool relative = webrtcManager && webrtcManager->relativeMouseMode.load();

    int x = 0;
    int y = 0;

    if (relative) {
        // 相对模式：直接转发 Interception 原始增量，不做阈值过滤(1px 也是有效输入)
        if (mousestroke.x != 0 || mousestroke.y != 0) {
            sendMouseRelativeEvent(mousestroke.x, mousestroke.y);
        }
        // 按键不带坐标(哨兵 -1)，避免 System 端 MouseButtonDown 内部绝对定位
        // 把游戏已锁定的光标 warp 到角落
        x = -1;
        y = -1;
    }
    else {
        // 绝对模式：取本机光标位置 -> 屏幕坐标
        POINT cursorPos;
        GetCursorPos(&cursorPos);

        // Convert to window client area coordinates
        POINT clientPt = cursorPos;
        if (targetHwnd) {
            ScreenToClient(targetHwnd, &clientPt);

            // Get window client area size
            RECT clientRect;
            GetClientRect(targetHwnd, &clientRect);
            int windowWidth  = clientRect.right - clientRect.left;
            int windowHeight = clientRect.bottom - clientRect.top;

            // Map window coordinates to screen coordinate system (maintain relative position)
            if (windowWidth > 0 && windowHeight > 0) {
                clientPt.x = (clientPt.x * screenWidth)  / windowWidth;
                clientPt.y = (clientPt.y * screenHeight) / windowHeight;

                // Boundary check
                if (clientPt.x < 0) clientPt.x = 0;
                if (clientPt.x >= screenWidth)  clientPt.x = screenWidth  - 1;
                if (clientPt.y < 0) clientPt.y = 0;
                if (clientPt.y >= screenHeight) clientPt.y = screenHeight - 1;
            }
        }

        x = clientPt.x;
        y = clientPt.y;

        // ===== 1+2 合并：取消计数器节流，改用 3 像素距离阈值 =====
        constexpr int kMoveThreshold2 = 2 * 2;          // 平方距离，省 sqrt
        int dx = x - lastMouseX.load();
        int dy = y - lastMouseY.load();
        if (dx * dx + dy * dy >= kMoveThreshold2) {
            sendMouseMoveEvent(x, y);
            lastMouseX = x;
            lastMouseY = y;
        }
    }

    // Process mouse buttons
    if (mousestroke.state & INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN) {
        sendMouseEvent(1, 0, x, y);
    }
    if (mousestroke.state & INTERCEPTION_MOUSE_LEFT_BUTTON_UP) {
        sendMouseEvent(2, 0, x, y);
    }
    if (mousestroke.state & INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN) {
        sendMouseEvent(1, 1, x, y);
    }
    if (mousestroke.state & INTERCEPTION_MOUSE_RIGHT_BUTTON_UP) {
        sendMouseEvent(2, 1, x, y);
    }
    if (mousestroke.state & INTERCEPTION_MOUSE_MIDDLE_BUTTON_DOWN) {
        sendMouseEvent(1, 2, x, y);
    }
    if (mousestroke.state & INTERCEPTION_MOUSE_MIDDLE_BUTTON_UP) {
        sendMouseEvent(2, 2, x, y);
    }

    // Process wheel
    if (mousestroke.state & INTERCEPTION_MOUSE_WHEEL) {
        sendWheelEvent(mousestroke.rolling);
    }
}

char InterceptionHook::getCurrentModifiers()
{
    char modifiers = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= 0x01;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= 0x02;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= 0x04;  // Alt key
    return modifiers;
}

bool InterceptionHook::isNumLockOn()
{
    return numLockState.load();
}

void InterceptionHook::sendKeyEvent(bool isPress, DWORD windowsVK, char modifiers)
{
    if (!webrtcManager) {
        return;
    }

    short type = isPress ? 3 : 4;  // 3=key down, 4=key up

#pragma pack(push,1)
    struct KeyButton {
        short type;
        DWORD buttonId;
        char modifiers;
    };
#pragma pack(pop)

    KeyButton* keyButton = new KeyButton{type, windowsVK, modifiers};
    webrtcManager->writerRemote(reinterpret_cast<unsigned char*>(keyButton), sizeof(KeyButton));
}

void InterceptionHook::sendMouseEvent(short type, short button, int x, int y)
{
    if (!webrtcManager) {
        return;
    }

    // x<0 / y<0 : 哨兵值(相对模式下按键不带坐标)，System 端跳过绝对定位
    uint32_t normalizedX;
    uint32_t normalizedY;
    if (x < 0 || y < 0) {
        normalizedX = 0xFFFFFFFFu;
        normalizedY = 0xFFFFFFFFu;
    } else {
        // Normalize coordinates to 0-65535 range
        normalizedX = (x << 16) / screenWidth;
        normalizedY = (y << 16) / screenHeight;
    }

#pragma pack(push,1)
    struct MouseButton {
        short type;
        short buttonId;
        uint32_t x;
        uint32_t y;
    };
#pragma pack(pop)

    MouseButton* mouseBtn = new MouseButton{type, button, normalizedX, normalizedY};
    webrtcManager->writerRemote(reinterpret_cast<unsigned char*>(mouseBtn), sizeof(MouseButton));
}

void InterceptionHook::sendMouseMoveEvent(int x, int y)
{
    if (!webrtcManager) return;

#pragma pack(push,1)
    struct MouseMove              // 6 字节
    {
        short  type;              // 0
        uint32_t x;               // 屏幕绝对像素
        uint32_t y;
    };
#pragma pack(pop)

    // 边界保护
    uint32_t normalizedX = (x << 16) / screenWidth;
    uint32_t normalizedY = (y << 16) / screenHeight;

    MouseMove* pkt = new MouseMove{0, normalizedX, normalizedY};

    webrtcManager->writerRemote(reinterpret_cast<unsigned char*>(pkt), sizeof(MouseMove));
}

void InterceptionHook::sendMouseRelativeEvent(int dx, int dy)
{
    if (!webrtcManager) return;

#pragma pack(push,1)
    struct MouseRelative         // 10 字节
    {
        short  type;              // 6
        uint32_t x;               // dx (按 int32 在 System 端解释)
        uint32_t y;               // dy
    };
#pragma pack(pop)

    MouseRelative* pkt = new MouseRelative{6, static_cast<uint32_t>(dx), static_cast<uint32_t>(dy)};

    webrtcManager->writerRemote(reinterpret_cast<unsigned char*>(pkt), sizeof(MouseRelative));
}

void InterceptionHook::sendWheelEvent(int delta)
{
    if (!webrtcManager) {
        return;
    }

#pragma pack(push,1)
    struct MouseWheel {
        short type;
        int x;
        int y;
    };
#pragma pack(pop)

    MouseWheel* mouseWheel = new MouseWheel{5, delta, 0};
    webrtcManager->writerRemote(reinterpret_cast<unsigned char*>(mouseWheel), sizeof(MouseWheel));
}

void InterceptionHook::convertClientToScreen(int& x, int& y)
{
    if (!targetHwnd) return;

    RECT clientRect;
    GetClientRect(targetHwnd, &clientRect);
    int windowWidth = clientRect.right - clientRect.left;
    int windowHeight = clientRect.bottom - clientRect.top;

    if (windowWidth > 0 && windowHeight > 0) {
        x = (x * screenWidth) / windowWidth;
        y = (y * screenHeight) / windowHeight;
    }
}


    }

}
