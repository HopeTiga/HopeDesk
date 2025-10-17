#ifndef WINDOWSHOOK_H
#define WINDOWSHOOK_H

#pragma once
#include <Windows.h>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <atomic>
#include <thread>
#include <mutex>

class WebRTCRemoteClient;
class VideoWidget;

class WindowsHook : public QObject
{
    Q_OBJECT

public:
    explicit WindowsHook(QObject* parent = nullptr);
    ~WindowsHook();

    // 设置目标窗口和客户端
    void setTargetWidget(VideoWidget* widget);
    void setRemoteClient(WebRTCRemoteClient* client);

    // 启动/停止Hook
    bool startHook();
    void stopHook();

    // 检查是否在目标窗口内
    bool isInTargetWindow() const;

    // 设置视频尺寸（保留接口兼容性）
    void setVideoSize(int width, int height);

private:
    // Hook回调函数
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    // Windows键码转Qt键码
    static int VKToQtKey(DWORD vkCode);

    // 获取当前修饰键状态
    static char getCurrentModifiers();

    // 坐标转换：将屏幕坐标转换为相对坐标
    QPoint screenToRelative(const QPoint& screenPos) const;

    // 发送事件到远程
    void sendKeyEvent(bool isPress, DWORD windowsVK, char modifiers);
    void sendMouseEvent(short type, short button, int x, int y);
    void sendMouseMoveEvent(int x, int y);
    void sendWheelEvent(int delta);

private:
    // 静态实例（Hook回调需要）
    static WindowsHook* sInstance;

    // Hook句柄
    HHOOK keyboardHook;
    HHOOK mouseHook;

    // 目标窗口
    VideoWidget* targetWidget;
    HWND targetHwnd;

    // 远程客户端
    WebRTCRemoteClient* remoteClient;

    // 屏幕尺寸
    std::atomic<int> screenWidth;
    std::atomic<int> screenHeight;

    // 鼠标位置优化
    QPoint lastMousePos;

    // Hook消息线程
    std::thread hookThread;
    std::atomic<bool> running;
};
#endif // WINDOWSHOOK_H
