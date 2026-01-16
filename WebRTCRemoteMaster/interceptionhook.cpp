#include "interceptionhook.h"
#include "videowidget.h"
#include "webrtcmanager.h"
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
    , manager(nullptr)
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

void InterceptionHook::setManager(WebRTCManager* manager)
{
    this->manager = manager;
    LOG_INFO("Remote client set");
}

void InterceptionHook::setVideoSize(int width, int height)
{

}

bool InterceptionHook::startCapture()
{
    if (running) {
        LOG_WARNING("Capture already running");
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
        device = interception_wait(context);
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
            if (isInTargetWindow()) {
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

    char modifiers = getCurrentModifiers();
    sendKeyEvent(isPress, vkCode, modifiers);
}

void InterceptionHook::processMouseEvent(InterceptionMouseStroke& mousestroke)
{
    // Get current mouse position
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

    int x = clientPt.x;
    int y = clientPt.y;

    // ===== 1+2 合并：取消计数器节流，改用 3 像素距离阈值 =====
    constexpr int kMoveThreshold2 = 2 * 2;          // 平方距离，省 sqrt
    int dx = x - lastMouseX.load();
    int dy = y - lastMouseY.load();
    if (dx * dx + dy * dy >= kMoveThreshold2) {
        sendMouseMoveEvent(x, y);
        lastMouseX = x;
        lastMouseY = y;
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
    if (!manager) {
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
    manager->writerRemote(reinterpret_cast<unsigned char*>(keyButton), sizeof(KeyButton));
}

void InterceptionHook::sendMouseEvent(short type, short button, int x, int y)
{
    if (!manager) {
        return;
    }

    // Normalize coordinates to 0-65535 range
    int normalizedX = (x << 16) / screenWidth;
    int normalizedY = (y << 16) / screenHeight;

#pragma pack(push,1)
    struct MouseButton {
        short type;
        short buttonId;
        int x;
        int y;
    };
#pragma pack(pop)

    MouseButton* mouseBtn = new MouseButton{type, button, normalizedX, normalizedY};
    manager->writerRemote(reinterpret_cast<unsigned char*>(mouseBtn), sizeof(MouseButton));
}

void InterceptionHook::sendMouseMoveEvent(int x, int y)
{
    if (!manager) return;

#pragma pack(push,1)
    struct MouseMove              // 6 字节
    {
        short  type;              // 0
        uint16_t x;               // 屏幕绝对像素
        uint16_t y;
        uint32_t sequence;
    };
#pragma pack(pop)

    // 边界保护
    uint16_t ux = static_cast<uint16_t>(std::clamp(x, 0, screenWidth));

    uint16_t uy = static_cast<uint16_t>(std::clamp(y, 0, screenHeight));

    MouseMove* pkt = new MouseMove{0, ux, uy,++mouseMoveSequence};

    manager->writerRemote(reinterpret_cast<unsigned char*>(pkt), sizeof(MouseMove));
}

void InterceptionHook::sendWheelEvent(int delta)
{
    if (!manager) {
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
    manager->writerRemote(reinterpret_cast<unsigned char*>(mouseWheel), sizeof(MouseWheel));
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
