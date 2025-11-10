#include "CursorHooks.h"
#include <iostream>
#include "Logger.h"
#include "Utils.h"


namespace hope {

    namespace rtc {
        // Static member definition
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

            // Run message loop in a separate thread
            hookThread = std::thread(&CursorHooks::hookThreadProc, this);

            // Wait for hook installation to complete
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        void CursorHooks::stopHooks()
        {
            if (!isRunning) {
                return;
            }

            isRunning = false;

            // Send exit message to hook thread
            if (hookThread.joinable()) {
                // Send WM_QUIT message to hook thread
                PostThreadMessage(GetThreadId(hookThread.native_handle()), WM_QUIT, 0, 0);
                hookThread.join();
            }

            instance = nullptr;
        }

        void CursorHooks::hookThreadProc()
        {
            // Install low-level mouse hook
            mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);

            if (!mouseHook) {
                Logger::getInstance()->error("Failed to install mouse hook, error code: " + std::to_string(GetLastError()));
                isRunning = false;
                return;
            }

            Logger::getInstance()->info("mouseHooks Installed");

            // Message loop (required, otherwise hook won't work)
            MSG msg;
            while (isRunning && GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // Uninstall hook
            if (mouseHook) {
                UnhookWindowsHookEx(mouseHook);
                mouseHook = nullptr;
                Logger::getInstance()->info("mouseHooks Uninstalled");
            }
        }

        LRESULT CALLBACK CursorHooks::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
        {
            if (nCode >= 0 && instance && instance->isRunning) {
                // Check cursor when mouse event occurs
                instance->checkCursorChange();
            }

            // Continue passing the message
            return CallNextHookEx(NULL, nCode, wParam, lParam);
        }

        void CursorHooks::checkCursorChange()
        {
            // Get current cursor
            CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
            if (!GetCursorInfo(&cursorInfo)) {
                return;
            }

            HCURSOR currentCursor = cursorInfo.hCursor;

            // Cursor unchanged, return directly
            if (currentCursor == lastCursor) {
                return;
            }

            lastCursor = currentCursor;

            Logger::getInstance()->debug("Detected new cursor: " + std::to_string(reinterpret_cast<uintptr_t>(currentCursor)));

#pragma pack(push,1)
            struct Cursors {
                short type;
                int index;
                int width;
                int height;
                int hotX;
                int hotY;
            };
#pragma pack(pop)

            if (cursorCaches.find(currentCursor) == cursorCaches.end()) {
                cursorCaches[currentCursor] = cursorCaches.size();

                // If handler exists, get cursor data and call it
                if (cursorHandler && currentCursor) {
                    unsigned char* bitmapData = nullptr;
                    size_t bitmapSize = 0;

                    getCursorBitmapData(currentCursor, bitmapData, bitmapSize);

                    if (bitmapData && bitmapSize > 0) {
                        int index = cursorCaches[currentCursor];

                        // Get cursor information
                        ICONINFO iconInfo = { 0 };
                        int hotX = 0, hotY = 0;
                        int width = 0, height = 0;

                        if (GetIconInfo(currentCursor, &iconInfo)) {
                            hotX = iconInfo.xHotspot;
                            hotY = iconInfo.yHotspot;

                            // Get width and height
                            if (iconInfo.hbmColor) {
                                BITMAP bm = { 0 };
                                GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bm);
                                width = bm.bmWidth;
                                height = bm.bmHeight;
                            }
                            else if (iconInfo.hbmMask) {
                                BITMAP bm = { 0 };
                                GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bm);
                                width = bm.bmWidth;
                                height = bm.bmHeight / 2;
                            }

                            Logger::getInstance()->debug("Cursor hotspot: (" + std::to_string(hotX) + ", " + std::to_string(hotY) + ")");
                            Logger::getInstance()->debug("Cursor size: " + std::to_string(width) + "x" + std::to_string(height));

                            // Clean up ICONINFO resources
                            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
                            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
                        }

                        cursorHotPos.emplace_back(hotX, hotY);
                        cursorSizes.emplace_back(width, height);

                        // Allocate memory: struct + bitmap data
                        size_t totalSize = sizeof(Cursors) + bitmapSize;
                        unsigned char* finalData = new unsigned char[totalSize];

                        // Write struct directly to buffer
                        Cursors* cursors = reinterpret_cast<Cursors*>(finalData);
                        cursors->type = 1;  // New cursor data
                        cursors->index = index;
                        cursors->width = width;
                        cursors->height = height;
                        cursors->hotX = hotX;
                        cursors->hotY = hotY;

                        // Copy bitmap data after struct
                        fastCopy(finalData + sizeof(Cursors), bitmapData, bitmapSize);

                        if (cursorHandler) {
                            cursorHandler(finalData, totalSize);
                        }

                        delete[] bitmapData;
                    }
                }
            }
            else {
                // Cached cursor, send index only
                int index = cursorCaches[currentCursor];

                // Bounds check
                if (index >= cursorHotPos.size() || index >= cursorSizes.size()) {
                    Logger::getInstance()->error("Invalid cursor cache index: " + std::to_string(index));
                    return;
                }

                std::pair<int, int> cursorPos = cursorHotPos[index];
                std::pair<int, int> cursorSize = cursorSizes[index];

                // Allocate only struct size
                unsigned char* data = new unsigned char[sizeof(Cursors)];

                // Operate directly using struct pointer
                Cursors* cursors = reinterpret_cast<Cursors*>(data);
                cursors->type = 0;  // Cursor index message
                cursors->index = index;
                cursors->width = cursorSize.first;
                cursors->height = cursorSize.second;
                cursors->hotX = cursorPos.first;
                cursors->hotY = cursorPos.second;

                if (cursorHandler) {
                    cursorHandler(data, sizeof(Cursors));
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
                Logger::getInstance()->error("GetIconInfo failed, error code: " + std::to_string(GetLastError()));
                return;
            }

            // Get bitmap information
            if (iconInfo.hbmColor) {
                GetObject(iconInfo.hbmColor, sizeof(BITMAP), &bmColor);
                hasColor = true;
            }

            if (iconInfo.hbmMask) {
                GetObject(iconInfo.hbmMask, sizeof(BITMAP), &bmMask);
            }

            // Use size from color bitmap or mask bitmap
            width = hasColor ? bmColor.bmWidth : bmMask.bmWidth;
            height = hasColor ? bmColor.bmHeight : bmMask.bmHeight / 2;

            if (width <= 0 || height <= 0) {
                Logger::getInstance()->warning("Invalid cursor size: " + std::to_string(width) + "x" + std::to_string(height));
                goto cleanup;
            }

            Logger::getInstance()->debug("Cursor size: " + std::to_string(width) + "x" + std::to_string(height));

            // Calculate required memory size
            size = width * height * bytesPerPixel;
            data = new unsigned char[size];
            memset(data, 0, size);

            // Get bitmap data
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
                    // Color cursor - Get original colors directly
                    if (GetDIBits(hdcMem, iconInfo.hbmColor, 0, height, data, &bmi, DIB_RGB_COLORS)) {
                        Logger::getInstance()->debug("Successfully got color cursor data (preserving original colors)");
                    }
                    else {
                        Logger::getInstance()->error("GetDIBits failed, error code: " + std::to_string(GetLastError()));
                        delete[] data;
                        data = nullptr;
                        size = 0;
                        goto cleanup;
                    }
                }
                else {
                    // Monochrome cursor handling
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
                        // Handle monochrome cursor - Preserve original black and white mode
                        for (int i = 0; i < width * height; i++) {
                            int idx = i * 4;
                            int xorIdx = (i + width * height) * 4; // XOR mask in lower half

                            if (maskData[xorIdx] > 0) {
                                // XOR white -> Display white
                                data[idx] = 255;     // R
                                data[idx + 1] = 255; // G
                                data[idx + 2] = 255; // B
                                data[idx + 3] = 255; // A
                            }
                            else {
                                int andIdx = i * 4;
                                if (maskData[andIdx] > 0) {
                                    // AND white -> Transparent
                                    data[idx + 3] = 0; // A
                                }
                                else {
                                    // AND black -> Display black (not white)
                                    data[idx] = 0;       // R
                                    data[idx + 1] = 0;   // G
                                    data[idx + 2] = 0;   // B
                                    data[idx + 3] = 255; // A
                                }
                            }
                        }
                        Logger::getInstance()->debug("Successfully got monochrome cursor data (preserving original black and white)");

                        // ====== Add black border ======
                        // Create temporary buffer to store original data
                        unsigned char* tempData = new unsigned char[size];
                        fastCopy(tempData, data, size);

                        for (int y = 0; y < height; y++) {
                            for (int x = 0; x < width; x++) {
                                int idx = (y * width + x) * 4;

                                // Process only transparent pixels
                                if (tempData[idx + 3] == 0) {
                                    bool hasOpaqueNeighbor = false;

                                    // Check 8 neighboring directions
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

                                    // If has opaque neighbor, add pure black border
                                    if (hasOpaqueNeighbor) {
                                        // Set pure black in RGB format
                                        data[idx] = 0;       // R
                                        data[idx + 1] = 0;   // G
                                        data[idx + 2] = 0;   // B
                                        data[idx + 3] = 255; // A
                                    }
                                }
                            }
                        }
                        delete[] tempData;
                        Logger::getInstance()->debug("Black border added (RGB)");
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
            }

        cleanup:
            if (hdcMem) DeleteDC(hdcMem);
            if (hdc) ReleaseDC(NULL, hdc);
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
        }
    }
}

