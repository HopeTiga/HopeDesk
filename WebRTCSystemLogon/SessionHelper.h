#pragma once

// WinSock 初始化冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <Wtsapi32.h>
#include <SecurityBaseApi.h>
#include <userenv.h> 
#include <string>
#include <stdexcept>
#include <vector>


// 一些常量定义，确保它们已经在 Windows.h 中定义了
// 如果在 Windows.h 没有定义它们的情况下进行定义
#ifndef TOKEN_ALL_ACCESS
#define TOKEN_ALL_ACCESS 0xF01FF
#endif

#ifndef MAXIMUM_ALLOWED
#define MAXIMUM_ALLOWED 0x02000000
#endif

class SessionHelper {
public:
    // 检查当前进程是否在活动终端会话中
    static bool CheckActiveTerminalSession();

    // 获取活动终端会话ID
    static DWORD GetActiveTerminalSessionId();

    // 在活动终端会话中重启当前应用程序（保持原有方法）
    static void RespawnInActiveTerminalSession();
    static void  RespawnInActiveTerminalSessionWithArgs(const std::wstring& args);

    // *** 新增：创建System级别的进程在用户会话中，支持WGC ***
    static HANDLE CreateSystemProcessInUserSession(const std::wstring& args);

private:
    // *** System Token 相关方法 ***
    // 获取System Token
    static HANDLE GetSystemToken();

    // 查找System进程ID
    static DWORD FindSystemProcessId();

    // 检查Token是否为System Token
    static bool IsSystemToken(HANDLE token);

    // 为Token启用所有必要的权限（包括WGC所需权限）
    static void EnableTokenPrivileges(HANDLE token);

    // 启用单个权限
    static bool EnablePrivilege(HANDLE token, const std::wstring& privilegeName);

    // 验证进程权限级别
    static void VerifyProcessPrivileges(DWORD processId);

    // 异常类，用于处理Win32 API错误
    class WinApiException : public std::runtime_error {
    public:
        WinApiException(const std::string& message) : std::runtime_error(message + ": " + GetLastErrorAsString()) {}

    private:
        std::string GetLastErrorAsString();
    };
};