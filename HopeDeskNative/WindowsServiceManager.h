#include <windows.h>
#include <tlhelp32.h>  // 添加进程枚举所需头文件
#include <string>
#include <iostream>
#include "Utils.h"  // 添加Logger头文件

namespace hope{

namespace rtc{

class WindowsServiceManager {
public:
    // 注册服务
    // 改进的服务注册函数
    static bool registerService(const std::string& serviceName, const std::string& exePath) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            LOG_ERROR("Failed to open service control manager: %d", GetLastError());
            return false;
        }

        // 构建服务命令行
        std::string serviceCommand = "\"" + exePath + "\"";

        // 创建服务 - 改为手动启动
        SC_HANDLE serviceHandle = CreateServiceA(
            serviceControlManager,
            serviceName.c_str(),
            serviceName.c_str(),
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS, // 添加交互标志
            SERVICE_DEMAND_START, // 改为手动启动，不是SERVICE_AUTO_START
            SERVICE_ERROR_NORMAL,
            serviceCommand.c_str(),
            nullptr, nullptr, nullptr, nullptr, nullptr
            );

        bool isSuccess = false;
        if (!serviceHandle) {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_EXISTS) {
                LOG_INFO("Service already exists: %s", serviceName.c_str());
                // 尝试打开现有服务并修改配置
                serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
                if (serviceHandle) {
                    // 修改现有服务配置
                    ChangeServiceConfigA(serviceHandle,
                                         SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                                         SERVICE_DEMAND_START, // 改为手动启动
                                         SERVICE_NO_CHANGE,
                                         serviceCommand.c_str(),
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                    isSuccess = true;
                }
            }
            else {
                LOG_ERROR("Failed to create service: %d", errorCode);
            }
        }
        else {
            LOG_INFO("Service registered successfully: %s", serviceName.c_str());

            // 设置服务描述
            SERVICE_DESCRIPTION desc;
            desc.lpDescription = (LPWSTR)"System Screen Capture Service for screen recording";
            ChangeServiceConfig2A(serviceHandle, SERVICE_CONFIG_DESCRIPTION, &desc);

            // 设置失败操作 - 重启服务
            SERVICE_FAILURE_ACTIONS failureActions;
            SC_ACTION actions[3] = {
                {SC_ACTION_RESTART, 5000},  // 5秒后重启
                {SC_ACTION_RESTART, 5000},  // 再次失败5秒后重启
                {SC_ACTION_NONE, 0}         // 第三次失败不操作
            };
            failureActions.dwResetPeriod = 86400; // 24小时重置计数器
            failureActions.lpRebootMsg = nullptr;
            failureActions.lpCommand = nullptr;
            failureActions.cActions = 3;
            failureActions.lpsaActions = actions;
            ChangeServiceConfig2A(serviceHandle, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);

            isSuccess = true;
        }

        if (serviceHandle) CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }

    // 查询服务是否存在
    static bool serviceExists(const std::string& serviceName) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!serviceControlManager) {
            LOG_ERROR("Failed to open service control manager: %d", GetLastError());
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_QUERY_STATUS);
        bool exists = (serviceHandle != nullptr);

        if (exists) {
            LOG_INFO("Service exists: %s", serviceName.c_str());
            CloseServiceHandle(serviceHandle);
        } else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_DOES_NOT_EXIST) {
                LOG_INFO("Service does not exist: %s", serviceName.c_str());
            } else {
                LOG_ERROR("Failed to query service: %s, Error: %d", serviceName.c_str(), errorCode);
            }
        }

        CloseServiceHandle(serviceControlManager);
        return exists;
    }

    // 启动服务
    static bool startService(const std::string& serviceName) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            LOG_ERROR("Failed to open service control manager: %d", GetLastError());
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!serviceHandle) {
            LOG_ERROR("Failed to open service: %s, Error: %d", serviceName.c_str(), GetLastError());
            CloseServiceHandle(serviceControlManager);
            return false;
        }

        bool isSuccess = StartServiceA(serviceHandle, 0, nullptr);
        if (isSuccess) {
            LOG_INFO("Service started successfully: %s", serviceName.c_str());
        }
        else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_ALREADY_RUNNING) {
                LOG_INFO("Service is already running: %s", serviceName.c_str());
                isSuccess = true;
            }
            else {
                LOG_ERROR("Failed to start service: %s, Error: %d", serviceName.c_str(), errorCode);
            }
        }

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }

    // 停止服务 - 强制杀死所有同名进程
    static bool stopService(const std::string& serviceName) {
        LOG_INFO("Force killing all processes with name: %s", serviceName.c_str());

        // 获取进程快照
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            LOG_ERROR("Failed to create process snapshot: %d", GetLastError());
            return false;
        }

        PROCESSENTRY32W processEntry;  // 使用宽字符版本
        processEntry.dwSize = sizeof(PROCESSENTRY32W);

        int killedCount = 0;

        // 遍历所有进程
        if (Process32FirstW(snapshot, &processEntry)) {  // 使用宽字符版本
            do {
                // 将宽字符转换为窄字符
                char processNameBuffer[MAX_PATH];
                WideCharToMultiByte(CP_ACP, 0, processEntry.szExeFile, -1,
                                    processNameBuffer, MAX_PATH, NULL, NULL);
                std::string processName = processNameBuffer;

                // 移除.exe扩展名进行比较
                size_t pos = processName.find(".exe");
                if (pos != std::string::npos) {
                    processName = processName.substr(0, pos);
                }

                // 比较进程名（不区分大小写）
                if (_stricmp(processName.c_str(), serviceName.c_str()) == 0) {
                    // 打开进程句柄
                    HANDLE processHandle = OpenProcess(PROCESS_TERMINATE, FALSE, processEntry.th32ProcessID);
                    if (processHandle) {
                        // 强制终止进程
                        if (TerminateProcess(processHandle, 0)) {
                            LOG_INFO("Successfully killed process: %s (PID: %d)", processNameBuffer, processEntry.th32ProcessID);
                            killedCount++;
                        } else {
                            LOG_ERROR("Failed to terminate process PID %d: %d", processEntry.th32ProcessID, GetLastError());
                        }
                        CloseHandle(processHandle);
                    } else {
                        LOG_ERROR("Failed to open process PID %d: %d", processEntry.th32ProcessID, GetLastError());
                    }
                }
            } while (Process32NextW(snapshot, &processEntry));  // 使用宽字符版本
        }

        CloseHandle(snapshot);

        if (killedCount > 0) {
            LOG_INFO("Total processes killed: %d", killedCount);
            return true;
        } else {
            LOG_INFO("No processes found with name: %s", serviceName.c_str());
            return true; // 没有找到进程也算成功
        }
    }

    // 删除服务
    static bool deleteService(const std::string& serviceName) {
        // 先尝试停止服务
        stopService(serviceName);

        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            LOG_ERROR("Failed to open service control manager: %d", GetLastError());
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!serviceHandle) {
            LOG_INFO("Service not found: %s", serviceName.c_str());
            CloseServiceHandle(serviceControlManager);
            return true; // 服务不存在也算成功
        }

        bool isSuccess = ::DeleteService(serviceHandle);
        if (isSuccess) {
            LOG_INFO("Service deleted successfully: %s", serviceName.c_str());
        }
        else {
            LOG_ERROR("Failed to delete service: %s, Error: %d", serviceName.c_str(), GetLastError());
        }

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }
};

}
}
