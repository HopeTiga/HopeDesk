#include "ProtectProcess.h"
#include "Utils.h"
#include <filesystem>

#ifdef _WIN32
#else
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <cstdlib>
#endif

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
#ifdef _WIN32
            if (hasChildProcess.load()) {
                if (childProcessInfo.hProcess) {
                    if (TerminateProcess(childProcessInfo.hProcess, 0)) {
                        LOG_INFO("Child process terminated successfully");
                    }
                    else {
                        DWORD error = GetLastError();
                        LOG_ERROR("Failed to terminate child process, error code: %lu", error);
                    }

                    WaitForSingleObject(childProcessInfo.hProcess, 5000);

                    CloseHandle(childProcessInfo.hProcess);
                    CloseHandle(childProcessInfo.hThread);

                    childProcessInfo = { 0 };
                    hasChildProcess.store(false);
                }
            }
#else
            if (hasChildProcess.load() && childPid > 0) {
                if (kill(childPid, SIGTERM) == 0) {
                    LOG_INFO("Sent SIGTERM to child process (pid: %d)", childPid);
                }
                else {
                    LOG_ERROR("Failed to send SIGTERM to child process (pid: %d)", childPid);
                }

                int status = 0;
                pid_t ret = waitpid(childPid, &status, 0);
                if (ret > 0) {
                    LOG_INFO("Child process exited");
                }
                else if (ret == -1) {
                    LOG_ERROR("waitpid failed for pid: %d", childPid);
                }

                childPid = -1;
                hasChildProcess.store(false);
            }
#endif
        }




#include <filesystem>  // 需要 C++17 或以上
        // 或者使用 Windows API

        void ProtectProcess::createProcess(const std::string& exePath)
        {
            if (hasChildProcess.load()) {
                killChildProcess();
            }

            std::string absolutePath = exePath;

            try {
                std::filesystem::path path(exePath);
                if (!path.is_absolute()) {
                    absolutePath = std::filesystem::absolute(path).string();
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Failed to convert path: %s - %s", exePath.c_str(), e.what());
                return;
            }

            LOG_INFO("Original path: %s", exePath.c_str());
            LOG_INFO("Absolute path: %s", absolutePath.c_str());

            // 检查文件是否存在
            if (!std::filesystem::exists(absolutePath)) {
                LOG_ERROR("Process not found: %s", absolutePath.c_str());
                return;
            }

            if (std::filesystem::is_directory(absolutePath)) {
                LOG_ERROR("Path is a directory: %s", absolutePath.c_str());
                return;
            }

            currentExePath = absolutePath;

            std::string::size_type pos = absolutePath.find_last_of("\\/");
            std::string childWorkingDir = (pos != std::string::npos) ? absolutePath.substr(0, pos) : "";

            LOG_INFO("Working directory: %s", childWorkingDir.c_str());

#ifdef _WIN32
            STARTUPINFOA si = { 0 };
            si.cb = sizeof(si);

            if (CreateProcessA(
                absolutePath.c_str(),
                NULL,
                NULL,
                NULL,
                FALSE,
                CREATE_NEW_CONSOLE,
                NULL,
                childWorkingDir.c_str(),
                &si,
                &childProcessInfo
            )) {
                LOG_INFO("Process created successfully: %s (PID: %lu)",
                    absolutePath.c_str(), childProcessInfo.dwProcessId);

                hasChildProcess.store(true);

                monitorThread = std::thread([this]() {
                    DWORD waitResult = WaitForSingleObject(childProcessInfo.hProcess, INFINITE);

                    if (waitResult == WAIT_OBJECT_0) {
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
#else
            pid_t pid = fork();
            if (pid == -1) {
                LOG_ERROR("Failed to fork: %s", strerror(errno));
                hasChildProcess.store(false);
                return;
            }

            if (pid == 0) {
                // 子进程
                if (!childWorkingDir.empty()) {
                    if (chdir(childWorkingDir.c_str()) != 0) {
                        LOG_ERROR("Failed to change working directory: %s", strerror(errno));
                        _exit(1);
                    }
                }
                execl(absolutePath.c_str(), absolutePath.c_str(), (char*)nullptr);
                LOG_ERROR("Failed to exec: %s", strerror(errno));
                _exit(1);
            }

            LOG_INFO("Process created successfully: %s (PID: %d)", absolutePath.c_str(), pid);
            childPid = pid;
            hasChildProcess.store(true);

            auto self = this;
            monitorThread = std::thread([self, pid]() {
                int status = 0;
                pid_t ret = waitpid(pid, &status, 0);

                if (ret > 0) {
                    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                    LOG_INFO("Child process exited with code: %d", exitCode);
                    self->hasChildProcess.store(false);

                    boost::asio::post(self->ioContext, [self]() {
                        LOG_WARNING("Child process terminated, attempting to restart...");
                        if (!self->currentExePath.empty()) {
                            self->createProcess(self->currentExePath);
                        }
                        });
                }
                else {
                    LOG_ERROR("waitpid failed for pid: %d", pid);
                    self->hasChildProcess.store(false);
                }
                });
            monitorThread.detach();
#endif
        }



    }

}