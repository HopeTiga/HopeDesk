#include "CursorHooks.h"
#include <iostream>

// 静态成员定义
CursorHooks* CursorHooks::instance = nullptr;

CursorHooks::~CursorHooks()
{
    stopHooks();
}

void CursorHooks::setCursorHandler(std::function<void(unsigned char*, size_t)> handler)
{
    this->cursorHandler = handler;
}

void CursorHooks::startHooks()
{
    if (isRunning) {
        return;
    }

    instance = this;
    isRunning = true;

    // 在独立线程中运行消息循环
    hookThread = std::thread(&CursorHooks::hookThreadProc, this);

    // 等待钩子安装完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void CursorHooks::stopHooks()
{
    if (!isRunning) {
        return;
    }

    isRunning = false;

    // 发送退出消息到钩子线程
    if (hookThread.joinable()) {
        // 向钩子线程发送WM_QUIT消息
        PostThreadMessage(GetThreadId(hookThread.native_handle()), WM_QUIT, 0, 0);
        hookThread.join();
    }

    instance = nullptr;
}

void CursorHooks::hookThreadProc()
{
    // 安装低级鼠标钩子
    mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);

    if (!mouseHook) {
        std::cout << "无法安装鼠标钩子，错误代码: " << GetLastError() << std::endl;
        isRunning = false;
        return;
    }

    std::cout << "鼠标钩子安装成功！" << std::endl;

    // 消息循环（必须的，否则钩子不工作）
    MSG msg;
    while (isRunning && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 卸载钩子
    if (mouseHook) {
        UnhookWindowsHookEx(mouseHook);
        mouseHook = nullptr;
        std::cout << "鼠标钩子已卸载" << std::endl;
    }
}

LRESULT CALLBACK CursorHooks::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && instance && instance->isRunning) {
        // 鼠标事件发生时检查光标
        instance->checkCursorChange();
    }

    // 继续传递消息
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}


void CursorHooks::checkCursorChange()
{
    // 获取当前光标
    CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&cursorInfo)) {
        return;
    }

    HCURSOR currentCursor = cursorInfo.hCursor;

    // 光标没变化，直接返回
    if (currentCursor == lastCursor) {
        return;
    }

    lastCursor = currentCursor;

    std::cout << "检测到新光标: " << currentCursor << std::endl;

    if (cursorCaches.find(currentCursor) == cursorCaches.end()) {

        cursorCaches[currentCursor] = cursorCaches.size();

        // 如果有处理器，获取光标数据并调用
        if (cursorHandler && currentCursor) {
            unsigned char* data = nullptr;
            size_t size = 0;

            getCursorBitmapData(currentCursor, data, size);

            if (data && size > 0) {

                short type = 1; // 新光标数据

                int index = cursorCaches[currentCursor];

                // 获取热点信息
                ICONINFO iconInfo = { 0 };
                int hotX = 0, hotY = 0;
                int width = 0, height = 0;

                if (GetIconInfo(currentCursor, &iconInfo)) {
                    hotX = iconInfo.xHotspot;
                    hotY = iconInfo.yHotspot;

                    // 获取宽度和高度
                    if (iconInfo.hbmColor) {
                        // 彩色光标
                        BITMAP bm = { 0 };
                        GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bm);
                        width = bm.bmWidth;
                        height = bm.bmHeight;
                    }
                    else if (iconInfo.hbmMask) {
                        // 单色光标
                        BITMAP bm = { 0 };
                        GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bm);
                        width = bm.bmWidth;
                        height = bm.bmHeight / 2;  // 单色光标掩码高度是实际高度的两倍
                    }

                    std::cout << "光标热点: (" << hotX << ", " << hotY << ")" << std::endl;
                    std::cout << "光标尺寸: " << width << "x" << height << std::endl;

                    // 清理ICONINFO资源
                    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
                    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
                }

                cursorHotPos.emplace_back(std::pair<int, int>(hotX, hotY));

                cursorSizes.emplace_back(std::pair<int, int>(width, height));

                // 增加两个int的大小用于存储热点
                size_t totalSize = sizeof(short) + sizeof(int) * 5 + size;

                unsigned char* finalData = new unsigned char[totalSize];

                std::memcpy(finalData, &type, sizeof(short));

                std::memcpy(finalData + sizeof(short), &index, sizeof(int));

                std::memcpy(finalData + sizeof(short) + sizeof(int), &width, sizeof(int));

                std::memcpy(finalData + sizeof(short) + sizeof(int) + sizeof(int), &height, sizeof(int));

                // 添加热点X坐标
                std::memcpy(finalData + sizeof(short) + sizeof(int) * 3, &hotX, sizeof(int));

                // 添加热点Y坐标  
                std::memcpy(finalData + sizeof(short) + sizeof(int) * 4, &hotY, sizeof(int));

                // 添加光标数据
                std::memcpy(finalData + sizeof(short) + sizeof(int) * 5, data, size);

                if (cursorHandler) {
                    cursorHandler(finalData, totalSize);
                }
                delete[] data;
            }
        }
    }
    else {

        int index = cursorCaches[currentCursor];

        std::pair<int, int> cursorPos = cursorHotPos[index];

        std::pair<int, int> cursorSize = cursorSizes[index];

        short type = 0;

        size_t size = sizeof(short) + sizeof(int) * 5;

        unsigned char* data = new unsigned char[size];

        std::memcpy(data, &type, sizeof(short));

        std::memcpy(data + sizeof(short), &index, sizeof(int));

        std::memcpy(data + sizeof(short) + sizeof(int), &cursorSize.first, sizeof(int));

        std::memcpy(data + sizeof(short) + sizeof(int) + sizeof(int), &cursorSize.second, sizeof(int));

        // 添加热点X坐标
        std::memcpy(data + sizeof(short) + sizeof(int) * 3, &cursorPos.first, sizeof(int));

        // 添加热点Y坐标  
        std::memcpy(data + sizeof(short) + sizeof(int) * 4, &cursorPos.second, sizeof(int));

        if (cursorHandler) {
            cursorHandler(data, size);
        }
    }
}



void CursorHooks::getCursorBitmapData(HCURSOR hCursor, unsigned char*& data, size_t& size)
{
    data = nullptr;
    size = 0;

    ICONINFO iconInfo = { 0 };
    BITMAP bmColor = { 0 };
    BITMAP bmMask = { 0 };
    HDC hdc = nullptr;
    HDC hdcMem = nullptr;
    int bytesPerPixel = 4;
    bool hasColor = false;
    int width = 0;
    int height = 0;

    if (!hCursor) {
        return;
    }

    if (!GetIconInfo(hCursor, &iconInfo)) {
        std::cout << "GetIconInfo失败，错误代码: " << GetLastError() << std::endl;
        return;
    }

    // 获取位图信息
    if (iconInfo.hbmColor) {
        GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmColor);
        hasColor = true;
    }

    if (iconInfo.hbmMask) {
        GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bmMask);
    }

    // 使用有颜色位图或掩码位图的尺寸
    width = hasColor ? bmColor.bmWidth : bmMask.bmWidth;
    height = hasColor ? bmColor.bmHeight : bmMask.bmHeight / 2;

    if (width <= 0 || height <= 0) {
        std::cout << "无效的光标尺寸: " << width << "x" << height << std::endl;
        goto cleanup;
    }

    std::cout << "光标尺寸: " << width << "x" << height << std::endl;

    // 计算需要的内存大小
    size = width * height * bytesPerPixel;
    data = new unsigned char[size];
    memset(data, 0, size);

    // 获取位图数据
    hdc = GetDC(NULL);
    hdcMem = CreateCompatibleDC(hdc);

    if (hdc && hdcMem) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        if (hasColor) {
            // 彩色光标 - 直接获取原始颜色
            if (GetDIBits(hdcMem, iconInfo.hbmColor, 0, height, data, &bmi, DIB_RGB_COLORS)) {
                std::cout << "成功获取彩色光标数据(保持原始颜色)" << std::endl;
            }
            else {
                std::cout << "GetDIBits失败，错误代码: " << GetLastError() << std::endl;
                delete[] data;
                data = nullptr;
                size = 0;
                goto cleanup;
            }
        }
        else {
            // 单色光标处理
            unsigned char* maskData = new unsigned char[width * bmMask.bmHeight * 4];
            memset(maskData, 0, width * bmMask.bmHeight * 4);

            BITMAPINFO bmiMask = {};
            bmiMask.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmiMask.bmiHeader.biWidth = width;
            bmiMask.bmiHeader.biHeight = -bmMask.bmHeight;
            bmiMask.bmiHeader.biPlanes = 1;
            bmiMask.bmiHeader.biBitCount = 32;
            bmiMask.bmiHeader.biCompression = BI_RGB;

            if (GetDIBits(hdcMem, iconInfo.hbmMask, 0, bmMask.bmHeight, maskData, &bmiMask, DIB_RGB_COLORS)) {
                // 处理单色光标 - 保持原始黑白模式
                for (int i = 0; i < width * height; i++) {
                    int idx = i * 4;
                    int xorIdx = (i + width * height) * 4; // XOR掩码在下半部分

                    if (maskData[xorIdx] > 0) {
                        // XOR白色 -> 显示白色
                        data[idx] = 255;     // R
                        data[idx + 1] = 255; // G
                        data[idx + 2] = 255; // B
                        data[idx + 3] = 255; // A
                    }
                    else {
                        int andIdx = i * 4;
                        if (maskData[andIdx] > 0) {
                            // AND白色 -> 透明
                            data[idx + 3] = 0; // A
                        }
                        else {
                            // AND黑色 -> 显示黑色（不是白色）
                            data[idx] = 0;       // R
                            data[idx + 1] = 0;   // G
                            data[idx + 2] = 0;   // B
                            data[idx + 3] = 255; // A
                        }
                    }
                }
                std::cout << "成功获取单色光标数据(保持原始黑白)" << std::endl;

                // ====== 添加黑色边框 ======
    // 创建临时缓冲区存储原始数据
                unsigned char* tempData = new unsigned char[size];
                memcpy(tempData, data, size);

                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int idx = (y * width + x) * 4;

                        // 只处理透明像素
                        if (tempData[idx + 3] == 0) {
                            bool hasOpaqueNeighbor = false;

                            // 检查8个方向的邻居
                            for (int dy = -1; dy <= 1; dy++) {
                                for (int dx = -1; dx <= 1; dx++) {
                                    if (dx == 0 && dy == 0) continue;

                                    int nx = x + dx;
                                    int ny = y + dy;

                                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                                        int nidx = (ny * width + nx) * 4;
                                        if (tempData[nidx + 3] > 0) {
                                            hasOpaqueNeighbor = true;
                                            break;
                                        }
                                    }
                                }
                                if (hasOpaqueNeighbor) break;
                            }

                            // 如果有不透明邻居，添加纯黑色边框
                            if (hasOpaqueNeighbor) {
                                // 使用RGB格式设置纯黑色
                                data[idx] = 0;       // R
                                data[idx + 1] = 0;   // G
                                data[idx + 2] = 0;   // B
                                data[idx + 3] = 255; // A
                            }
                        }
                    }
                }
                delete[] tempData;
                std::cout << "已添加黑色边框(RGB)" << std::endl;
            }
            else {
                delete[] data;
                data = nullptr;
                size = 0;
                delete[] maskData;
                goto cleanup;
            }
            delete[] maskData;
        }

        // 移除了人工添加黑色边框的代码 - 保持原始光标外观
    }

cleanup:
    if (hdcMem) DeleteDC(hdcMem);
    if (hdc) ReleaseDC(NULL, hdc);
    if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
}