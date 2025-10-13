#include "interceptionhook.h"
#include "videowidget.h"
#include "webrtcremoteclient.h"
#include "Logger.h"
#include <QWidget>
#include <QApplication>

InterceptionHook::InterceptionHook(QObject* parent)
    : QObject(parent)
    , context(nullptr)
    , keyboard(0)
    , mouse(0)
    , targetWidget(nullptr)
    , targetHwnd(nullptr)
    , remoteClient(nullptr)
    , running(false)
    , initialized(false)
    , lastMouseX(0)
    , lastMouseY(0)
    , numLockState(false)
{
    logger = Logger::getInstance();
    logger->info("InterceptionHook constructor");

    // 获取屏幕尺寸
    screenWidth = GetSystemMetrics(SM_CXSCREEN);
    screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

InterceptionHook::~InterceptionHook()
{
    stopCapture();
    logger->info("InterceptionHook destroyed");
}

void InterceptionHook::setTargetWidget(VideoWidget* widget)
{
    targetWidget = widget;
    if (widget) {
        targetHwnd = reinterpret_cast<HWND>(widget->winId());
        logger->info("Target widget set, HWND: " + std::to_string(reinterpret_cast<uintptr_t>(targetHwnd)));
    }
}

void InterceptionHook::setRemoteClient(WebRTCRemoteClient* client)
{
    remoteClient = client;
    logger->info("Remote client set");
}

void InterceptionHook::setVideoSize(int width, int height)
{
    // 保留接口兼容性
}

bool InterceptionHook::startCapture()
{
    if (running) {
        logger->warning("Capture already running");
        return true;
    }

    logger->info("Starting Interception capture...");

    // 创建 Interception 上下文
    context = interception_create_context();
    if (!context) {
        logger->error("Failed to create Interception context");
        logger->error("Please ensure:");
        logger->error("1. Running with administrator privileges");
        logger->error("2. Interception driver is installed");
        logger->error("3. Driver service is running");
        return false;
    }

    logger->info("Interception context created successfully");

    // 设置设备 ID
    keyboard = INTERCEPTION_KEYBOARD(0);
    mouse = INTERCEPTION_MOUSE(0);

    // 设置过滤器：捕获所有键盘和鼠标设备
    interception_set_filter(context, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);
    interception_set_filter(context, interception_is_mouse, INTERCEPTION_FILTER_MOUSE_ALL);

    initialized = true;
    running = true;

    // 启动捕获线程
    captureThread = std::thread(&InterceptionHook::captureThreadFunc, this);

    logger->info("Capture thread started");
    return true;
}

void InterceptionHook::stopCapture()
{
    if (!running) {
        return;
    }

    logger->info("Stopping capture...");
    running = false;

    // 等待线程结束
    if (captureThread.joinable()) {
        captureThread.join();
    }

    // 销毁上下文
    if (context) {
        interception_destroy_context(context);
        context = nullptr;
    }

    initialized = false;
    logger->info("Capture stopped");
}

bool InterceptionHook::isInTargetWindow() const
{
    if (!targetHwnd) {
        return false;
    }

    // 获取鼠标位置
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    // 获取鼠标所在的窗口
    HWND hwndUnderCursor = WindowFromPoint(cursorPos);

    // 检查是否是目标窗口或其子窗口
    return (hwndUnderCursor == targetHwnd || IsChild(targetHwnd, hwndUnderCursor));
}

void InterceptionHook::captureThreadFunc()
{
    logger->info("Capture thread started");
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

            // 全局监听NumLock按键
            bool isPress = !(keystroke->state & INTERCEPTION_KEY_UP);
            if (keystroke->code == 0x45 && isPress) {
                numLockState = !numLockState.load();
                logger->info("NumLock toggled to: " + std::string(numLockState ? "ON" : "OFF"));
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

    logger->info("Capture thread exiting");
}

void InterceptionHook::processKeyboardEvent(InterceptionKeyStroke& keystroke)
{
    bool isPress = !(keystroke.state & INTERCEPTION_KEY_UP);
    DWORD vkCode = MapVirtualKey(keystroke.code, MAPVK_VSC_TO_VK_EX);

    if (keystroke.state & INTERCEPTION_KEY_E0) {
        switch (keystroke.code) {
        case 0x1C: vkCode = VK_RETURN; break;
        case 0x1D: vkCode = VK_RCONTROL; break;
        case 0x35: vkCode = VK_DIVIDE; break;
        case 0x38: vkCode = VK_RMENU; break;
        case 0x47: vkCode = VK_HOME; break;
        case 0x48: vkCode = VK_UP; break;
        case 0x49: vkCode = VK_PRIOR; break;
        case 0x4B: vkCode = VK_LEFT; break;
        case 0x4D: vkCode = VK_RIGHT; break;
        case 0x4F: vkCode = VK_END; break;
        case 0x50: vkCode = VK_DOWN; break;
        case 0x51: vkCode = VK_NEXT; break;
        case 0x52: vkCode = VK_INSERT; break;
        case 0x53: vkCode = VK_DELETE; break;
        case 0x5B: vkCode = VK_LWIN; break;
        case 0x5C: vkCode = VK_RWIN; break;
        case 0x5D: vkCode = VK_APPS; break;
        }
    }
    else {
        if ((keystroke.code >= 0x47 && keystroke.code <= 0x53) || keystroke.code == 0x52) {
            bool numLockOn = isNumLockOn();

            if (numLockOn) {
                // NumLock ON: 发送数字键盘VK码
                switch (keystroke.code) {
                case 0x52: vkCode = VK_NUMPAD0; break;
                case 0x4F: vkCode = VK_NUMPAD1; break;
                case 0x50: vkCode = VK_NUMPAD2; break;
                case 0x51: vkCode = VK_NUMPAD3; break;
                case 0x4B: vkCode = VK_NUMPAD4; break;
                case 0x4C: vkCode = VK_NUMPAD5; break;
                case 0x4D: vkCode = VK_NUMPAD6; break;
                case 0x47: vkCode = VK_NUMPAD7; break;
                case 0x48: vkCode = VK_NUMPAD8; break;
                case 0x49: vkCode = VK_NUMPAD9; break;
                case 0x53: vkCode = VK_DECIMAL; break;
                }
            } else {
                // NumLock OFF: 发送主键盘数字键 0x30-0x39 ('0'-'9')
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
    }

    char modifiers = getCurrentModifiers();
    sendKeyEvent(isPress, vkCode, modifiers);
}

void InterceptionHook::processMouseEvent(InterceptionMouseStroke& mousestroke)
{
    // 获取当前鼠标位置
    POINT cursorPos;
    GetCursorPos(&cursorPos);

    // 转换为窗口客户区坐标
    POINT clientPt = cursorPos;
    if (targetHwnd) {
        ScreenToClient(targetHwnd, &clientPt);

        // 获取窗口客户区大小
        RECT clientRect;
        GetClientRect(targetHwnd, &clientRect);
        int windowWidth = clientRect.right - clientRect.left;
        int windowHeight = clientRect.bottom - clientRect.top;

        // 将窗口坐标映射到屏幕坐标系（保持相对位置）
        if (windowWidth > 0 && windowHeight > 0) {
            clientPt.x = (clientPt.x * screenWidth) / windowWidth;
            clientPt.y = (clientPt.y * screenHeight) / windowHeight;

            // 边界检查
            if (clientPt.x < 0) clientPt.x = 0;
            if (clientPt.x >= screenWidth) clientPt.x = screenWidth - 1;
            if (clientPt.y < 0) clientPt.y = 0;
            if (clientPt.y >= screenHeight) clientPt.y = screenHeight - 1;
        }
    }

    int x = clientPt.x;
    int y = clientPt.y;

    if (x != lastMouseX.load() || y != lastMouseY.load()) {
        sendMouseMoveEvent(x, y);
        lastMouseX = x;
        lastMouseY = y;
    }

    // 处理鼠标按键
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

    // 处理滚轮
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
    if (!remoteClient) {
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
    remoteClient->writerRemote(reinterpret_cast<unsigned char*>(keyButton), sizeof(KeyButton));
}

void InterceptionHook::sendMouseEvent(short type, short button, int x, int y)
{
    if (!remoteClient) {
        return;
    }

    // 将坐标归一化到 0-65535 范围
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
    remoteClient->writerRemote(reinterpret_cast<unsigned char*>(mouseBtn), sizeof(MouseButton));
}

void InterceptionHook::sendMouseMoveEvent(int x, int y)
{
    if (!remoteClient) {
        return;
    }

    // 将坐标归一化到 0-65535 范围
    int normalizedX = (x << 16) / screenWidth;
    int normalizedY = (y << 16) / screenHeight;

#pragma pack(push,1)
    struct MouseMove {
        short type;
        int x;
        int y;
    };
#pragma pack(pop)

    MouseMove* mouseMove = new MouseMove{0, normalizedX, normalizedY};
    remoteClient->writerRemote(reinterpret_cast<unsigned char*>(mouseMove), sizeof(MouseMove));
}

void InterceptionHook::sendWheelEvent(int delta)
{
    if (!remoteClient) {
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
    remoteClient->writerRemote(reinterpret_cast<unsigned char*>(mouseWheel), sizeof(MouseWheel));
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
