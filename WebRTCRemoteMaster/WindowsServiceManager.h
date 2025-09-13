#include <windows.h>
#include <string>
#include <iostream>
#include "Logger.h"  // 添加Logger头文件

class WindowsServiceManager {
public:
    // 注册服务
    // 改进的服务注册函数
    static bool registerService(const std::string& serviceName, const std::string& exePath) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            Logger::getInstance()->error("Failed to open service control manager: " + std::to_string(GetLastError()));
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
                Logger::getInstance()->info("Service already exists: " + serviceName);
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
                Logger::getInstance()->error("Failed to create service: " + std::to_string(errorCode));
            }
        }
        else {
            Logger::getInstance()->info("Service registered successfully: " + serviceName);

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
            Logger::getInstance()->error("Failed to open service control manager: " + std::to_string(GetLastError()));
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_QUERY_STATUS);
        bool exists = (serviceHandle != nullptr);

        if (exists) {
            Logger::getInstance()->info("Service exists: " + serviceName);
            CloseServiceHandle(serviceHandle);
        } else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_DOES_NOT_EXIST) {
                Logger::getInstance()->info("Service does not exist: " + serviceName);
            } else {
                Logger::getInstance()->error("Failed to query service: " + serviceName + ", Error: " + std::to_string(errorCode));
            }
        }

        CloseServiceHandle(serviceControlManager);
        return exists;
    }

    // 启动服务
    static bool startService(const std::string& serviceName) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            Logger::getInstance()->error("Failed to open service control manager: " + std::to_string(GetLastError()));
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!serviceHandle) {
            Logger::getInstance()->error("Failed to open service: " + serviceName + ", Error: " + std::to_string(GetLastError()));
            CloseServiceHandle(serviceControlManager);
            return false;
        }

        bool isSuccess = StartServiceA(serviceHandle, 0, nullptr);
        if (isSuccess) {
            Logger::getInstance()->info("Service started successfully: " + serviceName);
        }
        else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_ALREADY_RUNNING) {
                Logger::getInstance()->info("Service is already running: " + serviceName);
                isSuccess = true;
            }
            else {
                Logger::getInstance()->error("Failed to start service: " + serviceName + ", Error: " + std::to_string(errorCode));
            }
        }

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }

    // 停止服务
    static bool stopService(const std::string& serviceName) {
        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            Logger::getInstance()->error("Failed to open service control manager: " + std::to_string(GetLastError()));
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!serviceHandle) {
            Logger::getInstance()->error("Failed to open service: " + serviceName + ", Error: " + std::to_string(GetLastError()));
            CloseServiceHandle(serviceControlManager);
            return false;
        }

        SERVICE_STATUS serviceStatus;
        bool isSuccess = ControlService(serviceHandle, SERVICE_CONTROL_STOP, &serviceStatus);
        if (isSuccess) {
            Logger::getInstance()->info("Service stopped successfully: " + serviceName);
        }
        else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_SERVICE_NOT_ACTIVE) {
                Logger::getInstance()->info("Service is not running: " + serviceName);
                isSuccess = true;
            }
            else {
                Logger::getInstance()->error("Failed to stop service: " + serviceName + ", Error: " + std::to_string(errorCode));
            }
        }

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }

    // 删除服务
    static bool deleteService(const std::string& serviceName) {
        // 先尝试停止服务
        stopService(serviceName);

        SC_HANDLE serviceControlManager = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!serviceControlManager) {
            Logger::getInstance()->error("Failed to open service control manager: " + std::to_string(GetLastError()));
            return false;
        }

        SC_HANDLE serviceHandle = OpenServiceA(serviceControlManager, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!serviceHandle) {
            Logger::getInstance()->info("Service not found: " + serviceName);
            CloseServiceHandle(serviceControlManager);
            return true; // 服务不存在也算成功
        }

        bool isSuccess = ::DeleteService(serviceHandle);
        if (isSuccess) {
            Logger::getInstance()->info("Service deleted successfully: " + serviceName);
        }
        else {
            Logger::getInstance()->error("Failed to delete service: " + serviceName + ", Error: " + std::to_string(GetLastError()));
        }

        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(serviceControlManager);
        return isSuccess;
    }
};
