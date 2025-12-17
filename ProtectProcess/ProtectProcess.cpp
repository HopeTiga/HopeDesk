#include "ProtectProcess.h"
#include "Utils.h"
#include <filesystem>

namespace hope {

	namespace protect {


		ProtectProcess::ProtectProcess(boost::asio::io_context& ioContext)
			: ioContext(ioContext)
		{


		}

		ProtectProcess::~ProtectProcess()
		{
			killChildProcess();
		}

		void ProtectProcess::killChildProcess()
		{
			if (hasChildProcess.load()) {
				if (childProcessInfo.hProcess) {
					// 尝试优雅终止
					if (TerminateProcess(childProcessInfo.hProcess, 0)) {
						LOG_INFO("Child process terminated successfully");
					}
					else {
						DWORD error = GetLastError();
						LOG_ERROR("Failed to terminate child process, error code: %lu", error);
					}

					// 等待进程结束
					WaitForSingleObject(childProcessInfo.hProcess, 5000);

					CloseHandle(childProcessInfo.hProcess);
					CloseHandle(childProcessInfo.hThread);

					childProcessInfo = { 0 };
					hasChildProcess.store(false);
				}
			}
		}

		


#include <filesystem>  // 需要 C++17 或以上
        // 或者使用 Windows API

        void ProtectProcess::createProcess(const std::string& exePath)
        {
            // 如果已有子进程，先终止
            if (hasChildProcess.load()) {
                killChildProcess();
            }

            // 将相对路径转换为绝对路径
            std::string absolutePath = exePath;

            // 方法1: 使用 C++17 filesystem（推荐）
#if __cplusplus >= 201703L
            try {
                std::filesystem::path path(exePath);
                if (!path.is_absolute()) {
                    // 转换为绝对路径（相对于当前工作目录）
                    absolutePath = std::filesystem::absolute(path).string();
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Failed to convert path: %s - %s", exePath.c_str(), e.what());
                return;
            }
#else
// 方法2: 使用 Windows API
            char buffer[MAX_PATH];
            DWORD result = GetFullPathNameA(exePath.c_str(), MAX_PATH, buffer, NULL);
            if (result > 0 && result < MAX_PATH) {
                absolutePath = buffer;
            }
            else {
                LOG_ERROR("Failed to get full path for: %s", exePath.c_str());
                return;
            }
#endif

            LOG_INFO("Original path: %s", exePath.c_str());
            LOG_INFO("Absolute path: %s", absolutePath.c_str());

            // 检查文件是否存在
            DWORD fileAttr = GetFileAttributesA(absolutePath.c_str());
            if (fileAttr == INVALID_FILE_ATTRIBUTES) {
                DWORD error = GetLastError();
                LOG_ERROR("Process not found: %s (Error: %lu)", absolutePath.c_str(), error);
                return;
            }

            if (fileAttr & FILE_ATTRIBUTE_DIRECTORY) {
                LOG_ERROR("Path is a directory: %s", absolutePath.c_str());
                return;
            }

            // 保存可执行文件路径
            currentExePath = absolutePath;

            // 获取子进程自身的目录
            std::string::size_type pos = absolutePath.find_last_of("\\/");
            std::string childWorkingDir = (pos != std::string::npos) ? absolutePath.substr(0, pos) : "";

            LOG_INFO("Working directory: %s", childWorkingDir.c_str());

            STARTUPINFOA si = { 0 };
            si.cb = sizeof(si);

            // 创建进程 - 使用绝对路径
            if (CreateProcessA(
                absolutePath.c_str(),   // 使用绝对路径
                NULL,                   // 命令行参数
                NULL,                   // 进程安全属性
                NULL,                   // 线程安全属性
                FALSE,                  // 不继承句柄
                CREATE_NEW_CONSOLE,     // 创建新控制台
                NULL,                   // 使用父进程环境
                childWorkingDir.c_str(),// 指定子进程工作目录
                &si,                    // 启动信息
                &childProcessInfo       // 进程信息
            )) {
                LOG_INFO("Process created successfully: %s (PID: %lu)",
                    absolutePath.c_str(), childProcessInfo.dwProcessId);

                hasChildProcess.store(true);

                monitorThread = std::thread([this]() {
                    // 无限等待子进程退出
                    DWORD waitResult = WaitForSingleObject(childProcessInfo.hProcess, INFINITE);

                    if (waitResult == WAIT_OBJECT_0) {
                        // 子进程已退出
                        DWORD exitCode;
                        if (GetExitCodeProcess(childProcessInfo.hProcess, &exitCode)) {
                            LOG_INFO("Child process exited with code: %lu", exitCode);
                        }
                        hasChildProcess.store(false);

                        boost::asio::post(ioContext, [this]() {
                            LOG_WARNING("Child process terminated, attempting to restart...");
                            if (!currentExePath.empty()) {
                                createProcess(currentExePath);
                            }
                            });
                    }
                    else {
                        LOG_ERROR("Error waiting for child process. Error code: %lu", GetLastError());
                    }
                    });
                monitorThread.detach();
            }
            else {
                DWORD error = GetLastError();
                LOG_ERROR("Failed to create process: %s (Error code: %lu)",
                    absolutePath.c_str(), error);
                hasChildProcess.store(false);
            }
        }



	}

}