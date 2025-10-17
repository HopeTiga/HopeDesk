#pragma once
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include <functional>
#include <thread>

class ClipboardManager
{
public:

	ClipboardManager();

	~ClipboardManager();

	void setWriterAsyncHandle(std::function<void(unsigned char*, size_t)> handle);

	void start();

	void stop();

private:

	std::string WideToUTF8(const wchar_t* wideStr);
	
private:

	std::function<void(unsigned char*, size_t)> writerAsyncHandle;

	std::thread clipboardThread;

};

