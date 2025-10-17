#include "ClipboardManager.h"
#include "Logger.h"
#include <fstream>


ClipboardManager::ClipboardManager()
{
	winrt::init_apartment();

}

ClipboardManager::~ClipboardManager()
{
    stop();
}

void ClipboardManager::start()
{
    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::ContentChanged([this](winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&) {

        if (OpenClipboard(NULL)) {
            // 文本处理
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {

                HANDLE hData = GetClipboardData(CF_UNICODETEXT);

                if (hData != NULL) {

                    wchar_t* text = static_cast<wchar_t*>(GlobalLock(hData));

                    if (text != nullptr) {

                        std::string utf8Text = WideToUTF8(text);

                        unsigned char* data = new unsigned char[utf8Text.size()];

                        std::memcpy(data, utf8Text.data(), utf8Text.size());

                        GlobalUnlock(hData);

                        size_t dataSize = utf8Text.size();

                        if (this->writerAsyncHandle) {

                            this->writerAsyncHandle(data, dataSize);

                        }
                    }
                }
            }

            // 文件处理
            if (IsClipboardFormatAvailable(CF_HDROP)) {

                HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));

                if (hDrop != NULL) {

                    UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);

                    for (UINT i = 0; i < fileCount; i++) {

                        wchar_t filePath[MAX_PATH];

                        if (DragQueryFile(hDrop, i, filePath, MAX_PATH) > 0) {

                            std::string utf8Path = WideToUTF8(filePath);



                        }
                    }
                }
            }

            CloseClipboard();
        }
        else {
            Logger::getInstance()->error("[CLIPBOARD] Failed to open clipboard");
        }

        });
}

void ClipboardManager::stop()
{
    
}

std::string ClipboardManager::WideToUTF8(const wchar_t* wideStr)
{
    if (!wideStr) return "";

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, NULL, 0, NULL, NULL);
    if (size_needed == 0) return "";

    std::vector<char> buffer(size_needed);
    WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, buffer.data(), size_needed, NULL, NULL);
    return std::string(buffer.data());
}


void ClipboardManager::setWriterAsyncHandle(std::function<void(unsigned char*, size_t)> handle)
{
	this->writerAsyncHandle = handle;
}
