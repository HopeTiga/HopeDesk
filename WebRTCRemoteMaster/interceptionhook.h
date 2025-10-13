#ifndef INTERCEPTIONHOOK_H
#define INTERCEPTIONHOOK_H

#pragma once
#include <windows.h>
#include <interception/interception.h>
#include <QObject>
#include <QPoint>
#include <atomic>
#include <thread>
#include <memory>

class WebRTCRemoteClient;
class VideoWidget;
class Logger;

class InterceptionHook : public QObject
{
    Q_OBJECT

public:
    explicit InterceptionHook(QObject* parent = nullptr);
    ~InterceptionHook();

    // 设置目标窗口和客户端
    void setTargetWidget(VideoWidget* widget);
    void setRemoteClient(WebRTCRemoteClient* client);

    // 启动/停止捕获
    bool startCapture();
    void stopCapture();

    // 检查是否在目标窗口内
    bool isInTargetWindow() const;

    // 设置视频尺寸（保留接口兼容性）
    void setVideoSize(int width, int height);

private:
    // 捕获线程主函数
    void captureThreadFunc();

    // 处理键盘事件
    void processKeyboardEvent(InterceptionKeyStroke& keystroke);

    // 处理鼠标事件
    void processMouseEvent(InterceptionMouseStroke& mousestroke);

    // 获取当前修饰键状态
    static char getCurrentModifiers();
    
    // 检查NumLock状态
    bool isNumLockOn();

    // 发送事件到远程
    void sendKeyEvent(bool isPress, DWORD windowsVK, char modifiers);
    void sendMouseEvent(short type, short button, int x, int y);
    void sendMouseMoveEvent(int x, int y);
    void sendWheelEvent(int delta);

    // 坐标转换：窗口客户区坐标转屏幕坐标
    void convertClientToScreen(int& x, int& y);

private:
    // Interception 上下文
    InterceptionContext context;
    InterceptionDevice keyboard;
    InterceptionDevice mouse;

    // 目标窗口
    VideoWidget* targetWidget;
    HWND targetHwnd;

    // 远程客户端
    WebRTCRemoteClient* remoteClient;

    // 捕获线程
    std::thread captureThread;
    std::atomic<bool> running;
    std::atomic<bool> initialized;

    // 日志
    Logger* logger;

    // 上次鼠标位置（用于优化）
    std::atomic<int> lastMouseX;
    std::atomic<int> lastMouseY;

    // 屏幕尺寸缓存
    int screenWidth;
    int screenHeight;

    std::atomic<bool> numLockState;
};

#endif // INTERCEPTIONHOOK_H
