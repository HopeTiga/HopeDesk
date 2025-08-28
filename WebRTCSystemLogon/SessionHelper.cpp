#include "SessionHelper.h"
#include <sstream>
#include <iostream>
#include <vector>
#include "Logger.h"

// 检查当前进程是否在活动终端会话中
bool SessionHelper::CheckActiveTerminalSession() {
    Logger::getInstance()->info("CheckActiveTerminalSession: 开始检查当前进程会话");

    DWORD currentSessionId = 0;
    DWORD processId = GetCurrentProcessId();

    Logger::getInstance()->debug("CheckActiveTerminalSession: 当前进程ID = " + std::to_string(processId));

    if (!ProcessIdToSessionId(processId, &currentSessionId)) {
        Logger::getInstance()->error("CheckActiveTerminalSession: ProcessIdToSessionId 失败");
        throw WinApiException("ProcessIdToSessionId");
    }

    Logger::getInstance()->debug("CheckActiveTerminalSession: 当前会话ID = " + std::to_string(currentSessionId));

    DWORD activeSessionId = GetActiveTerminalSessionId();
    Logger::getInstance()->debug("CheckActiveTerminalSession: 活动会话ID = " + std::to_string(activeSessionId));

    bool result = currentSessionId == activeSessionId;
    Logger::getInstance()->info("CheckActiveTerminalSession: 结果 = " + std::string(result ? "true" : "false"));

    return result;
}

// 创建System级别的进程在用户会话中，支持WGC
HANDLE SessionHelper::CreateSystemProcessInUserSession(const std::wstring& args) {
    Logger::getInstance()->info("CreateSystemProcessInUserSession: 开始创建System级别进程，参数长度=" + std::to_string(args.length()));

    HANDLE systemToken = nullptr;
    HANDLE duplicatedToken = nullptr;
    LPVOID pEnv = nullptr;

    try {
        // 第一步：获取System Token
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 尝试获取System Token");
        systemToken = GetSystemToken();

        if (!systemToken) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: 无法获取System Token");
            throw std::runtime_error("Failed to get System Token");
        }
        Logger::getInstance()->info("CreateSystemProcessInUserSession: 成功获取System Token");

        // 第二步：获取活动用户会话ID
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("CreateSystemProcessInUserSession: 目标会话ID = " + std::to_string(sessionId));

        // 第三步：复制Token并设置会话ID
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 复制System Token");
        if (!DuplicateTokenEx(systemToken, MAXIMUM_ALLOWED, nullptr,
            SecurityIdentification, TokenPrimary, &duplicatedToken)) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: DuplicateTokenEx 失败");
            throw WinApiException("DuplicateTokenEx");
        }
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: DuplicateTokenEx 成功");

        // 设置Token的会话信息
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 设置Token会话信息");
        if (!SetTokenInformation(duplicatedToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: SetTokenInformation 失败");
            throw WinApiException("SetTokenInformation");
        }
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: SetTokenInformation 成功");

        // 第四步：为System Token启用所有权限（包括WGC所需权限）
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 启用Token权限");
        EnableTokenPrivileges(duplicatedToken);

        // 第五步：创建环境块
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 创建环境块");
        if (!CreateEnvironmentBlock(&pEnv, duplicatedToken, TRUE)) {
            Logger::getInstance()->warning("CreateSystemProcessInUserSession: CreateEnvironmentBlock 失败，继续执行");
            pEnv = nullptr;
        }
        else {
            Logger::getInstance()->debug("CreateSystemProcessInUserSession: CreateEnvironmentBlock 成功");
        }

        // 第六步：获取当前进程可执行文件路径
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 获取当前可执行文件路径");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: GetModuleFileName 失败");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("CreateSystemProcessInUserSession: 可执行文件路径 = " + exePathMB);

        // 第七步：构建命令行
        std::wstring commandLine = std::wstring(exePath);
        if (!args.empty()) {
            commandLine += L" " + args;
            std::string argsMB(args.begin(), args.end());
            Logger::getInstance()->info("CreateSystemProcessInUserSession: 命令行参数 = " + argsMB);
        }

        // 第八步：设置启动信息，指定桌面为用户桌面
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 初始化STARTUPINFO");
        STARTUPINFOW startupInfo = { 0 };
        startupInfo.cb = sizeof(startupInfo);
        // 重要：指定用户桌面，这样才能访问图形界面和WGC
        startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

        PROCESS_INFORMATION processInformation = { 0 };

        // 创建可变的命令行副本
        std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
        cmdLine.push_back(0);

        // 设置创建标志
        DWORD creationFlags = 0;  // 创建新控制台
        creationFlags = CREATE_NO_WINDOW;  // 不创建新窗口
        if (pEnv) {
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            Logger::getInstance()->debug("CreateSystemProcessInUserSession: 使用Unicode环境标志");
        }

        Logger::getInstance()->info("CreateSystemProcessInUserSession: 调用CreateProcessAsUserW，创建标志=" + std::to_string(creationFlags));

        // 第九步：创建进程
        if (!CreateProcessAsUserW(
            duplicatedToken,    // 使用复制的System Token
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            creationFlags,
            pEnv,
            nullptr,
            &startupInfo,
            &processInformation)) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: CreateProcessAsUserW 失败");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("CreateSystemProcessInUserSession: CreateProcessAsUserW 成功，新进程ID=" + std::to_string(processInformation.dwProcessId));

        // 验证新进程的权限级别
        VerifyProcessPrivileges(processInformation.dwProcessId);

        // 关闭进程和线程句柄
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 关闭进程和线程句柄");
        HANDLE hProcess = processInformation.hProcess;
        CloseHandle(processInformation.hThread);

        Logger::getInstance()->info("CreateSystemProcessInUserSession: 成功完成");

        return hProcess;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("CreateSystemProcessInUserSession: 捕获异常 - " + std::string(e.what()));
        // 清理资源
        if (pEnv) DestroyEnvironmentBlock(pEnv);
        if (duplicatedToken) CloseHandle(duplicatedToken);
        if (systemToken) CloseHandle(systemToken);
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("CreateSystemProcessInUserSession: 捕获未知异常");
        // 清理资源
        if (pEnv) DestroyEnvironmentBlock(pEnv);
        if (duplicatedToken) CloseHandle(duplicatedToken);
        if (systemToken) CloseHandle(systemToken);
        throw;
    }

    // 清理资源
    if (pEnv) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 销毁环境块");
        DestroyEnvironmentBlock(pEnv);
    }
    if (duplicatedToken) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 关闭duplicatedToken句柄");
        CloseHandle(duplicatedToken);
    }
    if (systemToken) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: 关闭systemToken句柄");
        CloseHandle(systemToken);
    }

    return nullptr; // 如果发生异常，返回nullptr
}

// 获取System Token
HANDLE SessionHelper::GetSystemToken() {
    Logger::getInstance()->info("GetSystemToken: 开始获取System Token");

    HANDLE processHandle = nullptr;
    HANDLE tokenHandle = nullptr;

    try {
        // 方法1：尝试从当前进程获取（如果当前进程是system权限）
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tokenHandle)) {
            if (IsSystemToken(tokenHandle)) {
                Logger::getInstance()->info("GetSystemToken: 当前进程已是System权限");
                return tokenHandle;
            }
            CloseHandle(tokenHandle);
            tokenHandle = nullptr;
        }

        // 方法2：尝试从System进程获取Token
        Logger::getInstance()->debug("GetSystemToken: 尝试从System进程获取Token");

        // 查找System进程（通常是ID为4的System进程）
        DWORD systemProcessId = FindSystemProcessId();
        if (systemProcessId == 0) {
            Logger::getInstance()->error("GetSystemToken: 无法找到System进程");
            throw std::runtime_error("Cannot find System process");
        }

        Logger::getInstance()->debug("GetSystemToken: System进程ID = " + std::to_string(systemProcessId));

        // 打开System进程
        processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, systemProcessId);
        if (!processHandle) {
            Logger::getInstance()->error("GetSystemToken: 无法打开System进程");
            throw WinApiException("OpenProcess");
        }

        // 获取System进程的Token
        if (!OpenProcessToken(processHandle, TOKEN_DUPLICATE | TOKEN_QUERY, &tokenHandle)) {
            Logger::getInstance()->error("GetSystemToken: 无法获取System进程Token");
            throw WinApiException("OpenProcessToken");
        }

        Logger::getInstance()->info("GetSystemToken: 成功获取System Token");
        CloseHandle(processHandle);
        return tokenHandle;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("GetSystemToken: 异常 - " + std::string(e.what()));
        if (processHandle) CloseHandle(processHandle);
        if (tokenHandle) CloseHandle(tokenHandle);
        throw;
    }
}

// 查找System进程ID
DWORD SessionHelper::FindSystemProcessId() {
    Logger::getInstance()->debug("FindSystemProcessId: 开始查找System进程");

    // System进程通常ID为4，但我们也要检查其他可能的system进程
    std::vector<DWORD> candidateIds = { 4, 0 }; // System进程通常是ID 4

    for (DWORD pid : candidateIds) {
        if (pid == 0) continue;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (hProcess) {
            HANDLE hToken = nullptr;
            if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                if (IsSystemToken(hToken)) {
                    Logger::getInstance()->info("FindSystemProcessId: 找到System进程，ID = " + std::to_string(pid));
                    CloseHandle(hToken);
                    CloseHandle(hProcess);
                    return pid;
                }
                CloseHandle(hToken);
            }
            CloseHandle(hProcess);
        }
    }

    Logger::getInstance()->warning("FindSystemProcessId: 未找到System进程");
    return 0;
}

// 检查Token是否为System Token
bool SessionHelper::IsSystemToken(HANDLE token) {
    TOKEN_USER* tokenUser = nullptr;
    DWORD returnLength = 0;

    try {
        // 获取Token用户信息
        GetTokenInformation(token, TokenUser, nullptr, 0, &returnLength);
        tokenUser = (TOKEN_USER*)malloc(returnLength);

        if (!GetTokenInformation(token, TokenUser, tokenUser, returnLength, &returnLength)) {
            free(tokenUser);
            return false;
        }

        // 检查是否为SYSTEM SID
        PSID systemSid = nullptr;
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

        if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
            0, 0, 0, 0, 0, 0, 0, &systemSid)) {
            free(tokenUser);
            return false;
        }

        bool isSystem = EqualSid(tokenUser->User.Sid, systemSid);

        FreeSid(systemSid);
        free(tokenUser);

        return isSystem;
    }
    catch (...) {
        if (tokenUser) free(tokenUser);
        return false;
    }
}

// 为Token启用所有必要的权限（包括WGC所需权限）
void SessionHelper::EnableTokenPrivileges(HANDLE token) {
    Logger::getInstance()->debug("EnableTokenPrivileges: 开始启用Token权限");

    // WGC和图形访问所需的权限
    std::vector<std::wstring> privileges = {
        SE_DEBUG_NAME,                  // 调试权限
        SE_TCB_NAME,                   // 可信计算基础权限
        SE_ASSIGNPRIMARYTOKEN_NAME,    // 分配主要令牌权限
        SE_IMPERSONATE_NAME,           // 模拟权限
        SE_INCREASE_QUOTA_NAME,        // 增加配额权限
        SE_CHANGE_NOTIFY_NAME,         // 更改通知权限
        SE_SECURITY_NAME,              // 安全权限
        SE_TAKE_OWNERSHIP_NAME,        // 取得所有权权限
        SE_LOAD_DRIVER_NAME,           // 加载驱动程序权限
        SE_SYSTEM_PROFILE_NAME,        // 系统配置文件权限
        SE_SYSTEMTIME_NAME,            // 系统时间权限
        SE_PROF_SINGLE_PROCESS_NAME,   // 单一进程配置文件权限
        SE_INC_BASE_PRIORITY_NAME,     // 增加基本优先级权限
        SE_CREATE_PAGEFILE_NAME,       // 创建页面文件权限
        SE_CREATE_PERMANENT_NAME,      // 创建永久对象权限
        SE_BACKUP_NAME,                // 备份权限
        SE_RESTORE_NAME,               // 还原权限
        SE_SHUTDOWN_NAME,              // 关机权限
        SE_AUDIT_NAME,                 // 审核权限
        SE_SYSTEM_ENVIRONMENT_NAME,    // 系统环境权限
        SE_UNDOCK_NAME,                // 从扩展坞移除权限
        SE_MANAGE_VOLUME_NAME,         // 管理卷权限
        SE_RELABEL_NAME,               // 重新标记权限
        SE_INC_WORKING_SET_NAME,       // 增加工作集权限
        SE_TIME_ZONE_NAME,             // 时区权限
        SE_CREATE_SYMBOLIC_LINK_NAME   // 创建符号链接权限
    };

    for (const auto& privilege : privileges) {
        EnablePrivilege(token, privilege);
    }

    Logger::getInstance()->info("EnableTokenPrivileges: 权限启用完成");
}

// 启用单个权限
bool SessionHelper::EnablePrivilege(HANDLE token, const std::wstring& privilegeName) {
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValue(nullptr, privilegeName.c_str(), &luid)) {
        Logger::getInstance()->debug("EnablePrivilege: LookupPrivilegeValue 失败 - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
        Logger::getInstance()->debug("EnablePrivilege: AdjustTokenPrivileges 失败 - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        Logger::getInstance()->debug("EnablePrivilege: 权限未分配 - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    Logger::getInstance()->debug("EnablePrivilege: 成功启用权限 - " +
        std::string(privilegeName.begin(), privilegeName.end()));
    return true;
}

// 验证进程权限级别
void SessionHelper::VerifyProcessPrivileges(DWORD processId) {
    Logger::getInstance()->debug("VerifyProcessPrivileges: 验证进程权限，PID = " + std::to_string(processId));

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (!hProcess) {
        Logger::getInstance()->warning("VerifyProcessPrivileges: 无法打开进程");
        return;
    }

    HANDLE hToken = nullptr;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
        if (IsSystemToken(hToken)) {
            Logger::getInstance()->info("VerifyProcessPrivileges: 确认进程具有System权限");
        }
        else {
            Logger::getInstance()->warning("VerifyProcessPrivileges: 进程不具有System权限");
        }
        CloseHandle(hToken);
    }

    CloseHandle(hProcess);
}

void  SessionHelper::RespawnInActiveTerminalSessionWithArgs(const std::wstring& args) {
    Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 开始重启进程，参数长度=" + std::to_string(args.length()));

    HANDLE token = nullptr;
    HANDLE newToken = nullptr;
    LPVOID pEnv = nullptr;

    try {
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 获取活动终端会话ID");
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 目标会话ID = " + std::to_string(sessionId));

        // Get the user token for the active session
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 尝试获取用户令牌");
        if (!WTSQueryUserToken(sessionId, &token)) {
            Logger::getInstance()->warning("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken 失败，回退到当前进程令牌方式");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 打开当前进程令牌");
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken 失败");
                throw WinApiException("OpenProcessToken");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken 成功");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 复制令牌");
            if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                SecurityIdentification,
                TokenPrimary, &newToken)) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx 失败");
                throw WinApiException("DuplicateTokenEx");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx 成功");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 设置令牌会话信息");
            if (!SetTokenInformation(newToken, TokenSessionId,
                &sessionId, sizeof(sessionId))) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation 失败");
                throw WinApiException("SetTokenInformation");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation 成功");
        }
        else {
            Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken 成功，直接使用用户令牌");
            newToken = token;
            token = nullptr;  // Prevent double-close
        }

        // Create environment block for the user context
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 创建环境块");
        if (!CreateEnvironmentBlock(&pEnv, newToken, TRUE)) {
            Logger::getInstance()->warning("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock 失败，继续执行");
        }
        else {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock 成功");
        }

        // Get current process executable path
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 获取当前可执行文件路径");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: GetModuleFileName 失败");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 可执行文件路径 = " + exePathMB);

        // Build command line with arguments
        std::wstring commandLine = std::wstring(exePath);
        if (!args.empty()) {
            commandLine += L" " + args;
            std::string argsMB(args.begin(), args.end());
            Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 命令行参数 = " + argsMB);
        }

        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 初始化STARTUPINFO");
        STARTUPINFOW startupInfo = { 0 };
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

        PROCESS_INFORMATION processInformation = { 0 };

        // Create mutable copy of command line
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 准备命令行参数");
        std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
        cmdLine.push_back(0);

        // Use CREATE_UNICODE_ENVIRONMENT flag when we have an environment block
        DWORD creationFlags = 0;
        creationFlags = CREATE_NO_WINDOW;  // 不创建新窗口
        if (pEnv) {
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 使用Unicode环境标志");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 调用CreateProcessAsUserW，创建标志=" + std::to_string(creationFlags));
        if (!CreateProcessAsUserW(
            newToken,
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            creationFlags,
            pEnv,
            nullptr,
            &startupInfo,
            &processInformation)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW 失败");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW 成功，新进程ID=" + std::to_string(processInformation.dwProcessId));

        // Close process and thread handles
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 关闭进程和线程句柄");

        CloseHandle(processInformation.hProcess);

        CloseHandle(processInformation.hThread);

        // Clean up environment block
        if (pEnv) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 销毁环境块");
            DestroyEnvironmentBlock(pEnv);
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: 成功完成");
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: 捕获异常 - " + std::string(e.what()));

        if (pEnv) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 异常处理 - 销毁环境块");
            DestroyEnvironmentBlock(pEnv);
        }
        if (token != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 异常处理 - 关闭token句柄");
            CloseHandle(token);
        }
        if (newToken != nullptr && newToken != token) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: 异常处理 - 关闭newToken句柄");
            CloseHandle(newToken);
        }
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: 捕获未知异常");

        if (pEnv) {
            DestroyEnvironmentBlock(pEnv);
        }
        if (token != nullptr) {
            CloseHandle(token);
        }
        if (newToken != nullptr && newToken != token) {
            CloseHandle(newToken);
        }
        throw;
    }

}

// 在活动终端会话中重启当前应用程序（无参数）
void SessionHelper::RespawnInActiveTerminalSession() {
    Logger::getInstance()->info("RespawnInActiveTerminalSession: 开始重启进程（无参数）");

    HANDLE token = nullptr;
    HANDLE newToken = nullptr;

    try {
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 获取活动终端会话ID");
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("RespawnInActiveTerminalSession: 目标会话ID = " + std::to_string(sessionId));

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 打开当前进程令牌");
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: OpenProcessToken 失败");
            throw WinApiException("OpenProcessToken");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: OpenProcessToken 成功");

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 复制令牌");
        if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
            SecurityIdentification,
            TokenPrimary, &newToken)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: DuplicateTokenEx 失败");
            throw WinApiException("DuplicateTokenEx");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: DuplicateTokenEx 成功");

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 设置令牌会话信息");
        if (!SetTokenInformation(newToken, TokenSessionId,
            &sessionId, sizeof(sessionId))) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: SetTokenInformation 失败");
            throw WinApiException("SetTokenInformation");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: SetTokenInformation 成功");

        // 获取当前进程的可执行文件路径
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 获取当前可执行文件路径");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: GetModuleFileName 失败");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("RespawnInActiveTerminalSession: 可执行文件路径 = " + exePathMB);

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 初始化STARTUPINFO");
        STARTUPINFOW startupInfo = { 0 };
        startupInfo.cb = sizeof(startupInfo);

        DWORD creationFlags = 0;
        creationFlags = CREATE_NO_WINDOW;  // 不创建新窗口


        PROCESS_INFORMATION processInformation = { 0 };

        Logger::getInstance()->info("RespawnInActiveTerminalSession: 调用CreateProcessAsUserW");
        if (!CreateProcessAsUserW(
            newToken,
            exePath,
            nullptr,
            nullptr,
            nullptr,
            false,
            creationFlags,
            nullptr,
            nullptr,
            &startupInfo,
            &processInformation)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: CreateProcessAsUserW 失败");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSession: CreateProcessAsUserW 成功，新进程ID=" + std::to_string(processInformation.dwProcessId));

        // 关闭进程和线程句柄
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: 关闭进程和线程句柄");
        CloseHandle(processInformation.hProcess);
        CloseHandle(processInformation.hThread);

        Logger::getInstance()->info("RespawnInActiveTerminalSession: 成功完成");
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("RespawnInActiveTerminalSession: 捕获异常 - " + std::string(e.what()));

        if (token != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSession: 异常处理 - 关闭token句柄");
            CloseHandle(token);
        }
        if (newToken != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSession: 异常处理 - 关闭newToken句柄");
            CloseHandle(newToken);
        }
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("RespawnInActiveTerminalSession: 捕获未知异常");

        if (token != nullptr) {
            CloseHandle(token);
        }
        if (newToken != nullptr) {
            CloseHandle(newToken);
        }
        throw;
    }
}

// 获取活动终端会话ID
DWORD SessionHelper::GetActiveTerminalSessionId() {
    Logger::getInstance()->info("GetActiveTerminalSessionId: 开始获取活动终端会话ID");

    PWTS_SESSION_INFO pSessionArray = nullptr;
    DWORD sessionCount = 0;

    try {
        Logger::getInstance()->debug("GetActiveTerminalSessionId: 调用WTSEnumerateSessions");
        if (!WTSEnumerateSessions(nullptr, 0, 1, &pSessionArray, &sessionCount)) {
            Logger::getInstance()->error("GetActiveTerminalSessionId: WTSEnumerateSessions 失败");
            throw WinApiException("WTSEnumerateSessions");
        }

        Logger::getInstance()->info("GetActiveTerminalSessionId: 找到 " + std::to_string(sessionCount) + " 个会话");

        DWORD activeSessionId = 0;
        bool sessionFound = false;

        // 遍历所有会话
        for (DWORD i = 0; i < sessionCount; i++) {
            WTS_SESSION_INFO session = pSessionArray[i];

            Logger::getInstance()->debug("GetActiveTerminalSessionId: 会话 " + std::to_string(i) +
                " - ID=" + std::to_string(session.SessionId) +
                ", 状态=" + std::to_string(session.State));

            if (session.State == WTSActive) {
                activeSessionId = session.SessionId;
                sessionFound = true;
                Logger::getInstance()->info("GetActiveTerminalSessionId: 找到活动会话ID = " + std::to_string(activeSessionId));
                break;
            }
        }

        WTSFreeMemory(pSessionArray);

        if (!sessionFound) {
            Logger::getInstance()->error("GetActiveTerminalSessionId: 未找到活动终端会话");
            throw std::runtime_error("Could not find active terminal session.");
        }

        Logger::getInstance()->info("GetActiveTerminalSessionId: 成功返回会话ID = " + std::to_string(activeSessionId));
        return activeSessionId;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("GetActiveTerminalSessionId: 捕获异常 - " + std::string(e.what()));
        if (pSessionArray != nullptr) {
            WTSFreeMemory(pSessionArray);
        }
        throw;
    }
}

// 获取最后一个错误的字符串表示
std::string SessionHelper::WinApiException::GetLastErrorAsString() {
    DWORD errorCode = GetLastError();
    Logger::getInstance()->debug("WinApiException::GetLastErrorAsString: 错误代码 = " + std::to_string(errorCode));

    if (errorCode == 0) {
        return "No error";
    }

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer,
        0,
        nullptr
    );

    std::string message(messageBuffer, size);
    Logger::getInstance()->debug("WinApiException::GetLastErrorAsString: 错误信息 = " + message);

    LocalFree(messageBuffer);
    return message;
}