#include "windowshook.h"
#include "videowidget.h"
#include "webrtcremoteclient.h"
#include <QWidget>
#include <QApplication>
#include <cstring>
#include <algorithm>

namespace hope{

namespace rtc{

// 静态成员初始化
WindowsHook* WindowsHook::sInstance = nullptr;

WindowsHook::WindowsHook(QObject* parent)
    : QObject(parent)
    , keyboardHook(nullptr)
    , mouseHook(nullptr)
    , targetWidget(nullptr)
    , targetHwnd(nullptr)
    , remoteClient(nullptr)
    , running(false)
{
    sInstance = this;
}

WindowsHook::~WindowsHook()
{
    stopHook();
    sInstance = nullptr;
}

void WindowsHook::setTargetWidget(VideoWidget* widget)
{
    targetWidget = widget;
    if (widget) {
        targetHwnd = reinterpret_cast<HWND>(widget->winId());
    }
}

void WindowsHook::setRemoteClient(WebRTCRemoteClient* client)
{
    remoteClient = client;
}

void WindowsHook::setVideoSize(int width, int height)
{
    // 保留接口兼容性
}

bool WindowsHook::startHook()
{
    if (running) {
        return true;
    }

    running = true;

    // 在单独的线程中运行Hook消息循环
    hookThread = std::thread([this]() {

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        // 安装键盘Hook
        keyboardHook = SetWindowsHookEx(
            WH_KEYBOARD_LL,
            LowLevelKeyboardProc,
            GetModuleHandle(NULL),
            0
            );

        // 安装鼠标Hook
        mouseHook = SetWindowsHookEx(
            WH_MOUSE_LL,
            LowLevelMouseProc,
            GetModuleHandle(NULL),
            0
            );

        if (!keyboardHook || !mouseHook) {
            qDebug() << "Failed to install hooks";
            running = false;
            return;
        }

        // 消息循环
        MSG msg;
        while (running && GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // 卸载Hook
        if (keyboardHook) {
            UnhookWindowsHookEx(keyboardHook);
            keyboardHook = nullptr;
        }
        if (mouseHook) {
            UnhookWindowsHookEx(mouseHook);
            mouseHook = nullptr;
        }
    });

    return true;
}

void WindowsHook::stopHook()
{
    if (!running) {
        return;
    }

    running = false;

    // 发送退出消息到Hook线程
    if (hookThread.joinable()) {
        PostThreadMessage(GetThreadId(hookThread.native_handle()), WM_QUIT, 0, 0);
        hookThread.join();
    }
}

bool WindowsHook::isInTargetWindow() const
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
    return (hwndUnderCursor == targetHwnd ||
            IsChild(targetHwnd, hwndUnderCursor));
}

void WindowsHook::sendKeyEvent(bool isPress, DWORD windowsVK, char modifiers)
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

    KeyButton * keyButton = new KeyButton{type,windowsVK,modifiers};

    remoteClient->writerRemote(reinterpret_cast<unsigned char *>(keyButton), sizeof(KeyButton));
}

void WindowsHook::sendMouseEvent(short type, short button, int x, int y)
{
    if (!remoteClient) {
        return;
    }

    // 将坐标归一化到 0-65535 范围
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

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

    MouseButton * mouseBtn = new MouseButton{type,button,normalizedX,normalizedY};

    remoteClient->writerRemote(reinterpret_cast<unsigned char * >(mouseBtn), sizeof(MouseButton));
}

// 修改 sendMouseMoveEvent 函数
void WindowsHook::sendMouseMoveEvent(int x, int y)
{
    if (!remoteClient) {
        return;
    }

    // 将坐标归一化到 0-65535 范围
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int normalizedX = (x << 16) / screenWidth;
    int normalizedY = (y << 16) / screenHeight;

#pragma pack(push,1)

    struct MouseMove {
        short type;
        int x;
        int y;
    };

#pragma pack(pop)

    MouseMove * mouseMove = new MouseMove{0,normalizedX,normalizedY};

    remoteClient->writerRemote(reinterpret_cast<unsigned char *>(mouseMove), sizeof(MouseMove));
}

void WindowsHook::sendWheelEvent(int delta)
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

    MouseWheel * mouseWheel = new MouseWheel{5,delta};

    remoteClient->writerRemote(reinterpret_cast<unsigned char*>(mouseWheel), sizeof(MouseWheel));
}

char WindowsHook::getCurrentModifiers()
{
    char modifiers = 0;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= 0x01;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= 0x02;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= 0x04;  // Alt key
    return modifiers;
}

// 键盘Hook回调
LRESULT CALLBACK WindowsHook::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && sInstance && sInstance->isInTargetWindow()) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;

        bool isPress = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isRelease = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        if (isPress || isRelease) {
            // Send raw Windows VK code directly, no conversion
            DWORD windowsVK = kbStruct->vkCode;
            char modifiers = getCurrentModifiers();

            sInstance->sendKeyEvent(isPress, windowsVK, modifiers);

            // Block event from reaching target window
            return 1;
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// 鼠标Hook回调 - 转换坐标处理窗口偏移，但不拦截事件
LRESULT CALLBACK WindowsHook::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && sInstance && sInstance->isInTargetWindow()) {
        MSLLHOOKSTRUCT* mouseStruct = (MSLLHOOKSTRUCT*)lParam;

        // 获取屏幕坐标
        POINT screenPt = { mouseStruct->pt.x, mouseStruct->pt.y };

        // 转换为窗口客户区坐标（处理窗口偏移）
        POINT clientPt = screenPt;
        if (sInstance->targetHwnd) {
            ScreenToClient(sInstance->targetHwnd, &clientPt);

            // 获取窗口客户区大小
            RECT clientRect;
            GetClientRect(sInstance->targetHwnd, &clientRect);
            int windowWidth = clientRect.right - clientRect.left;
            int windowHeight = clientRect.bottom - clientRect.top;

            // 获取屏幕大小
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);

            // 将窗口坐标映射到屏幕坐标系（保持相对位置）
            if (windowWidth > 0 && windowHeight > 0) {
                clientPt.x = (clientPt.x * screenWidth) / windowWidth;
                clientPt.y = (clientPt.y * screenHeight) / windowHeight;

                if (clientPt.x < 0) clientPt.x = 0;
                if (clientPt.x >= screenWidth) clientPt.x = screenWidth - 1;
                if (clientPt.y < 0) clientPt.y = 0;
                if (clientPt.y >= screenHeight) clientPt.y = screenHeight - 1;
            }
        }

        int x = clientPt.x;
        int y = clientPt.y;

        switch (wParam) {
        case WM_LBUTTONDOWN:
            sInstance->sendMouseEvent(1, 0, x, y);
            break;  // 不拦截，继续传递

        case WM_LBUTTONUP:
            sInstance->sendMouseEvent(2, 0, x, y);
            break;

        case WM_RBUTTONDOWN:
            sInstance->sendMouseEvent(1, 1, x, y);
            break;

        case WM_RBUTTONUP:
            sInstance->sendMouseEvent(2, 1, x, y);
            break;

        case WM_MBUTTONDOWN:
            sInstance->sendMouseEvent(1, 2, x, y);
            break;

        case WM_MBUTTONUP:
            sInstance->sendMouseEvent(2, 2, x, y);
            break;

        case WM_MOUSEMOVE:
            sInstance->sendMouseMoveEvent(x, y);
            break;  // 不拦截，让鼠标继续移动

        case WM_MOUSEWHEEL:
        {
            short delta = GET_WHEEL_DELTA_WPARAM(mouseStruct->mouseData);
            sInstance->sendWheelEvent(delta);
            break;
        }
        }
    }

    // 继续传递事件，不拦截
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}


}

}
