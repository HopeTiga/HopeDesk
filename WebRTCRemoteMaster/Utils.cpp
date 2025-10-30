#include "Utils.h"

// 获取当前时间字符串，格式：2025-06-17 11::18::00
void get_timestamp(char* buffer, size_t size) {
    time_t rawtime;
    struct tm timeinfo;

    time(&rawtime);

    // 使用安全版本的 localtime
#ifdef _WIN32
    // Windows 平台使用 localtime_s
    if (localtime_s(&timeinfo, &rawtime) != 0) {
        // 如果转换失败，使用默认时间格式
        strncpy_s(buffer, size, "0000-00-00 00::00::00", _TRUNCATE);
        return;
    }
#else
    // Unix/Linux 平台使用 localtime_r
    if (localtime_r(&rawtime, &timeinfo) == NULL) {
        // 如果转换失败，使用默认时间格式
        strncpy(buffer, "0000-00-00 00::00::00", size - 1);
        buffer[size - 1] = '\0';
        return;
    }
#endif

    strftime(buffer, size, "%Y-%m-%d %H::%M::%S", &timeinfo);
}

// 获取日志级别字符串和颜色
void get_level_info(LogLevel level, const char** level_str, const char** color) {
    switch (level) {
    case LOG_INFO:
        *level_str = "INFO";
        *color = COLOR_GREEN;
        break;
    case LOG_WARNING:
        *level_str = "WARNING";
        *color = COLOR_YELLOW;
        break;
    case LOG_ERROR:
        *level_str = "ERROR";
        *color = COLOR_RED;
        break;
    case LOG_DEBUG:
        *level_str = "DEBUG";
        *color = COLOR_BLUE;
        break;
    default:
        *level_str = "UNKNOWN";
        *color = COLOR_RESET;
        break;
    }
}

// 带彩色日志输出函数
void log_message(LogLevel level, const char* format, ...) {
    char timestamp[32];
    const char* level_str;
    const char* color;
    va_list args;

    // 获取时间戳
    get_timestamp(timestamp, sizeof(timestamp));

    // 获取级别信息
    get_level_info(level, &level_str, &color);

    // 输出格式化的日志
    printf("%s[%s][%s]", color, level_str, timestamp);

    // 输出用户消息
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("%s\n", COLOR_RESET);
}

// 无颜色版本的日志输出
void log_message_plain(LogLevel level, const char* format, ...) {
    char timestamp[32];
    const char* level_str;
    const char* color;
    va_list args;

    get_timestamp(timestamp, sizeof(timestamp));
    get_level_info(level, &level_str, &color);

    printf("[%s][%s]", level_str, timestamp);

    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
}

HCURSOR CreateCursorFromRGBA(unsigned char* rgbaData, int width, int height, int hotX, int hotY)
{
    HDC hdc = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdc);
    ReleaseDC(NULL, hdc);
    if (!hdcMem) return NULL;

    // 1. 准备32位颜色位图
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBitmap) {
        DeleteDC(hdcMem);
        return NULL;
    }

    // 需要将RGB转换为BGR，因为Windows DIB期望BGR格式
    // 但是如果getCursorBitmapData输出的是RGB，我们需要转换
    for (int i = 0; i < width * height; i++) {
        int idx = i * 4;
        BYTE r = rgbaData[idx];     // R
        BYTE g = rgbaData[idx + 1]; // G
        BYTE b = rgbaData[idx + 2]; // B
        BYTE a = rgbaData[idx + 3]; // A

        // 转换为BGR格式写入DIB
        ((BYTE*)pBits)[idx] = r;     // B
        ((BYTE*)pBits)[idx + 1] = g; // G
        ((BYTE*)pBits)[idx + 2] = b; // R
        ((BYTE*)pBits)[idx + 3] = a; // A
    }

    // 2. 创建单色掩码位图（1位）
    HBITMAP hMask = CreateBitmap(width, height, 1, 1, NULL);
    if (!hMask) {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        return NULL;
    }

    // 3. 根据Alpha通道生成掩码
    // 掩码规则：0（黑色）= 不透明，1（白色）= 透明
    int maskRowBytes = ((width + 15) / 16) * 2;
    BYTE* maskBits = new BYTE[maskRowBytes * height];
    memset(maskBits, 0xFF, maskRowBytes * height); // 初始化为全白（全透明）

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            BYTE alpha = rgbaData[idx + 3];

            // Alpha > 128 的像素设为黑色（不透明）
            if (alpha > 128) {
                int byteIdx = y * maskRowBytes + (x / 8);
                int bitPos = 7 - (x % 8);
                maskBits[byteIdx] &= ~(1 << bitPos); // 清除位（设为0/黑色）
            }
        }
    }

    SetBitmapBits(hMask, maskRowBytes * height, maskBits);
    delete[] maskBits;

    // 4. 创建光标
    ICONINFO iconInfo = {};
    iconInfo.fIcon = FALSE; // 创建光标
    iconInfo.xHotspot = hotX;
    iconInfo.yHotspot = hotY;
    iconInfo.hbmMask = hMask;    // 单色掩码
    iconInfo.hbmColor = hBitmap; // 颜色位图

    HCURSOR cursor = CreateIconIndirect(&iconInfo);

    // 5. 清理资源
    DeleteObject(hBitmap);
    DeleteObject(hMask);
    DeleteDC(hdcMem);

    return cursor;
}
