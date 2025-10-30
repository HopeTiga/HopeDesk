#pragma once
#include <Windows.h>
#include <functional>
#include <atomic>
#include <thread>
#include <unordered_map>

class CursorHooks {
public:
    CursorHooks() = default;
    ~CursorHooks();

    void setCursorHandler(std::function<void(unsigned char*, size_t)> handler);
    void startHooks();
    void stopHooks();

private:
    // 低级鼠标钩子处理
    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    // 工作线程函数
    void hookThreadProc();

    // 获取光标位图数据
    void getCursorBitmapData(HCURSOR hCursor, unsigned char*& data, size_t& size);

    // 检查并处理光标变化
    void checkCursorChange();

private:
    static CursorHooks* instance;
    std::function<void(unsigned char*, size_t)> cursorHandler;
    std::atomic<bool> isRunning{ false };

    HHOOK mouseHook = nullptr;

    HCURSOR lastCursor = nullptr;

    std::thread hookThread;

    // 用于缓存已处理的光标，避免重复处理
    std::unordered_map<HCURSOR, int> cursorCaches;

    std::vector<std::pair<int, int>> cursorHotPos;

    std::vector<std::pair<int, int>> cursorSizes;
};