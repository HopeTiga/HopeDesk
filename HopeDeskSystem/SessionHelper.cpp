#include "SessionHelper.h"
#include <sstream>
#include <iostream>
#include <vector>
#include "Utils.h"

namespace hope {

    namespace rtc {

        // Check if the current process is in the active terminal session
        bool SessionHelper::CheckActiveTerminalSession() {
            DWORD currentSessionId = 0;
            DWORD processId = GetCurrentProcessId();

            if (!ProcessIdToSessionId(processId, &currentSessionId)) {
                LOG_ERROR("CheckActiveTerminalSession: ProcessIdToSessionId failed");
                throw WinApiException("ProcessIdToSessionId");
            }

            DWORD activeSessionId = GetActiveTerminalSessionId();
            return currentSessionId == activeSessionId;
        }

        // Create a System-level process in the user session, supporting WGC
        HANDLE SessionHelper::CreateSystemProcessInUserSession(const std::wstring& args) {
            HANDLE systemToken = nullptr;
            HANDLE duplicatedToken = nullptr;
            LPVOID pEnv = nullptr;

            try {
                // Step 1: Get System Token
                systemToken = GetSystemToken();
                if (!systemToken) {
                    LOG_ERROR("CreateSystemProcessInUserSession: Failed to get System Token");
                    throw std::runtime_error("Failed to get System Token");
                }

                // Step 2: Get active user session ID
                DWORD sessionId = GetActiveTerminalSessionId();

                // Step 3: Duplicate Token and set session ID
                if (!DuplicateTokenEx(systemToken, MAXIMUM_ALLOWED, nullptr,
                    SecurityIdentification, TokenPrimary, &duplicatedToken)) {
                    LOG_ERROR("CreateSystemProcessInUserSession: DuplicateTokenEx failed");
                    throw WinApiException("DuplicateTokenEx");
                }

                if (!SetTokenInformation(duplicatedToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
                    LOG_ERROR("CreateSystemProcessInUserSession: SetTokenInformation failed");
                    throw WinApiException("SetTokenInformation");
                }

                // Step 4: Enable all privileges for System Token
                EnableTokenPrivileges(duplicatedToken);

                // Step 5: Create environment block
                if (!CreateEnvironmentBlock(&pEnv, duplicatedToken, TRUE)) {
                    LOG_WARNING("CreateSystemProcessInUserSession: CreateEnvironmentBlock failed, continuing");
                    pEnv = nullptr;
                }

                // Step 6: Get current process executable path
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("CreateSystemProcessInUserSession: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                // Step 7: Build command line
                std::wstring commandLine = std::wstring(exePath);
                if (!args.empty()) {
                    commandLine += L" " + args;
                }

                // Step 8: Set startup information
                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);
                startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

                PROCESS_INFORMATION processInformation = { 0 };

                std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
                cmdLine.push_back(0);

                DWORD creationFlags = CREATE_NO_WINDOW;
                if (pEnv) {
                    creationFlags |= CREATE_UNICODE_ENVIRONMENT;
                }

                // Step 9: Create process
                if (!CreateProcessAsUserW(
                    duplicatedToken,
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
                    LOG_ERROR("CreateSystemProcessInUserSession: CreateProcessAsUserW failed");
                    throw WinApiException("CreateProcessAsUserW");
                }

                VerifyProcessPrivileges(processInformation.dwProcessId);

                HANDLE hProcess = processInformation.hProcess;
                CloseHandle(processInformation.hThread);

                return hProcess;
            }
            catch (const std::exception& e) {
                LOG_ERROR("CreateSystemProcessInUserSession: Caught exception - %s", e.what());
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (duplicatedToken) CloseHandle(duplicatedToken);
                if (systemToken) CloseHandle(systemToken);
                throw;
            }
            catch (...) {
                LOG_ERROR("CreateSystemProcessInUserSession: Caught unknown exception");
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (duplicatedToken) CloseHandle(duplicatedToken);
                if (systemToken) CloseHandle(systemToken);
                throw;
            }

            if (pEnv) DestroyEnvironmentBlock(pEnv);
            if (duplicatedToken) CloseHandle(duplicatedToken);
            if (systemToken) CloseHandle(systemToken);

            return nullptr;
        }

        // Get System Token
        HANDLE SessionHelper::GetSystemToken() {
            HANDLE processHandle = nullptr;
            HANDLE tokenHandle = nullptr;

            try {
                // Method 1: Try current process
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tokenHandle)) {
                    if (IsSystemToken(tokenHandle)) {
                        return tokenHandle;
                    }
                    CloseHandle(tokenHandle);
                    tokenHandle = nullptr;
                }

                // Method 2: System process
                DWORD systemProcessId = FindSystemProcessId();
                if (systemProcessId == 0) {
                    LOG_ERROR("GetSystemToken: Failed to find System process");
                    throw std::runtime_error("Cannot find System process");
                }

                processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, systemProcessId);
                if (!processHandle) {
                    LOG_ERROR("GetSystemToken: Failed to open System process");
                    throw WinApiException("OpenProcess");
                }

                if (!OpenProcessToken(processHandle, TOKEN_DUPLICATE | TOKEN_QUERY, &tokenHandle)) {
                    LOG_ERROR("GetSystemToken: Failed to get System process Token");
                    throw WinApiException("OpenProcessToken");
                }

                CloseHandle(processHandle);
                return tokenHandle;
            }
            catch (const std::exception& e) {
                LOG_ERROR("GetSystemToken: Exception - %s", e.what());
                if (processHandle) CloseHandle(processHandle);
                if (tokenHandle) CloseHandle(tokenHandle);
                throw;
            }
        }

        // Find System process ID
        DWORD SessionHelper::FindSystemProcessId() {
            std::vector<DWORD> candidateIds = { 4, 0 };

            for (DWORD pid : candidateIds) {
                if (pid == 0) continue;

                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (hProcess) {
                    HANDLE hToken = nullptr;
                    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                        if (IsSystemToken(hToken)) {
                            CloseHandle(hToken);
                            CloseHandle(hProcess);
                            return pid;
                        }
                        CloseHandle(hToken);
                    }
                    CloseHandle(hProcess);
                }
            }

            LOG_WARNING("FindSystemProcessId: System process not found");
            return 0;
        }

        // Check if Token is System Token
        bool SessionHelper::IsSystemToken(HANDLE token) {
            TOKEN_USER* tokenUser = nullptr;
            DWORD returnLength = 0;

            try {
                GetTokenInformation(token, TokenUser, nullptr, 0, &returnLength);
                tokenUser = (TOKEN_USER*)malloc(returnLength);

                if (!GetTokenInformation(token, TokenUser, tokenUser, returnLength, &returnLength)) {
                    free(tokenUser);
                    return false;
                }

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

        // Enable all necessary privileges for Token
        void SessionHelper::EnableTokenPrivileges(HANDLE token) {
            std::vector<std::wstring> privileges = {
                SE_DEBUG_NAME, SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME,
                SE_IMPERSONATE_NAME, SE_INCREASE_QUOTA_NAME, SE_CHANGE_NOTIFY_NAME,
                SE_SECURITY_NAME, SE_TAKE_OWNERSHIP_NAME, SE_LOAD_DRIVER_NAME,
                SE_SYSTEM_PROFILE_NAME, SE_SYSTEMTIME_NAME, SE_PROF_SINGLE_PROCESS_NAME,
                SE_INC_BASE_PRIORITY_NAME, SE_CREATE_PAGEFILE_NAME, SE_CREATE_PERMANENT_NAME,
                SE_BACKUP_NAME, SE_RESTORE_NAME, SE_SHUTDOWN_NAME, SE_AUDIT_NAME,
                SE_SYSTEM_ENVIRONMENT_NAME, SE_UNDOCK_NAME, SE_MANAGE_VOLUME_NAME,
                SE_RELABEL_NAME, SE_INC_WORKING_SET_NAME, SE_TIME_ZONE_NAME,
                SE_CREATE_SYMBOLIC_LINK_NAME
            };

            for (const auto& privilege : privileges) {
                EnablePrivilege(token, privilege);
            }
        }

        // Enable a single privilege
        bool SessionHelper::EnablePrivilege(HANDLE token, const std::wstring& privilegeName) {
            TOKEN_PRIVILEGES tp;
            LUID luid;

            if (!LookupPrivilegeValue(nullptr, privilegeName.c_str(), &luid)) {
                return false;
            }

            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

            if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
                return false;
            }

            if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                return false;
            }

            return true;
        }

        // Verify process privilege level
        void SessionHelper::VerifyProcessPrivileges(DWORD processId) {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
            if (!hProcess) {
                return;
            }

            HANDLE hToken = nullptr;
            if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                IsSystemToken(hToken);
                CloseHandle(hToken);
            }

            CloseHandle(hProcess);
        }

        void SessionHelper::RespawnInActiveTerminalSessionWithArgs(const std::wstring& args) {
            HANDLE token = nullptr;
            HANDLE newToken = nullptr;
            LPVOID pEnv = nullptr;

            try {
                DWORD sessionId = GetActiveTerminalSessionId();

                if (!WTSQueryUserToken(sessionId, &token)) {
                    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken failed");
                        throw WinApiException("OpenProcessToken");
                    }

                    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                        SecurityIdentification, TokenPrimary, &newToken)) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx failed");
                        throw WinApiException("DuplicateTokenEx");
                    }

                    if (!SetTokenInformation(newToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation failed");
                        throw WinApiException("SetTokenInformation");
                    }
                }
                else {
                    newToken = token;
                    token = nullptr;
                }

                if (!CreateEnvironmentBlock(&pEnv, newToken, TRUE)) {
                    LOG_WARNING("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock failed");
                }

                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                std::wstring commandLine = std::wstring(exePath);
                if (!args.empty()) {
                    commandLine += L" " + args;
                }

                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);
                startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

                PROCESS_INFORMATION processInformation = { 0 };

                std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
                cmdLine.push_back(0);

                DWORD creationFlags = CREATE_NO_WINDOW;
                if (pEnv) {
                    creationFlags |= CREATE_UNICODE_ENVIRONMENT;
                }

                if (!CreateProcessAsUserW(
                    newToken, nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                    creationFlags, pEnv, nullptr, &startupInfo, &processInformation)) {
                    LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW failed");
                    throw WinApiException("CreateProcessAsUserW");
                }

                CloseHandle(processInformation.hProcess);
                CloseHandle(processInformation.hThread);

                if (pEnv) DestroyEnvironmentBlock(pEnv);
            }
            catch (const std::exception& e) {
                LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: Caught exception - %s", e.what());
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (token) CloseHandle(token);
                if (newToken && newToken != token) CloseHandle(newToken);
                throw;
            }
            catch (...) {
                LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: Caught unknown exception");
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (token) CloseHandle(token);
                if (newToken && newToken != token) CloseHandle(newToken);
                throw;
            }
        }

        void SessionHelper::RespawnInActiveTerminalSession() {
            HANDLE token = nullptr;
            HANDLE newToken = nullptr;

            try {
                DWORD sessionId = GetActiveTerminalSessionId();

                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                    LOG_ERROR("RespawnInActiveTerminalSession: OpenProcessToken failed");
                    throw WinApiException("OpenProcessToken");
                }

                if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                    SecurityIdentification, TokenPrimary, &newToken)) {
                    LOG_ERROR("RespawnInActiveTerminalSession: DuplicateTokenEx failed");
                    throw WinApiException("DuplicateTokenEx");
                }

                if (!SetTokenInformation(newToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
                    LOG_ERROR("RespawnInActiveTerminalSession: SetTokenInformation failed");
                    throw WinApiException("SetTokenInformation");
                }

                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("RespawnInActiveTerminalSession: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);

                DWORD creationFlags = CREATE_NO_WINDOW;
                PROCESS_INFORMATION processInformation = { 0 };

                if (!CreateProcessAsUserW(
                    newToken, exePath, nullptr, nullptr, nullptr, false,
                    creationFlags, nullptr, nullptr, &startupInfo, &processInformation)) {
                    LOG_ERROR("RespawnInActiveTerminalSession: CreateProcessAsUserW failed");
                    throw WinApiException("CreateProcessAsUserW");
                }

                CloseHandle(processInformation.hProcess);
                CloseHandle(processInformation.hThread);
            }
            catch (const std::exception& e) {
                LOG_ERROR("RespawnInActiveTerminalSession: Caught exception - %s", e.what());
                if (token) CloseHandle(token);
                if (newToken) CloseHandle(newToken);
                throw;
            }
            catch (...) {
                LOG_ERROR("RespawnInActiveTerminalSession: Caught unknown exception");
                if (token) CloseHandle(token);
                if (newToken) CloseHandle(newToken);
                throw;
            }
        }

        // Get active terminal session ID
        DWORD SessionHelper::GetActiveTerminalSessionId() {
            PWTS_SESSION_INFO pSessionArray = nullptr;
            DWORD sessionCount = 0;

            if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionArray, &sessionCount)) {
                LOG_ERROR("WTSEnumerateSessions failed");
                throw WinApiException("WTSEnumerateSessions");
            }

            DWORD targetId = 0;
            for (DWORD i = 0; i < sessionCount; ++i) {
                const auto& s = pSessionArray[i];

                if (s.State == WTSActive) {
                    targetId = s.SessionId;
                    break;
                }
                if (s.State == WTSConnected && s.SessionId != 0) {
                    targetId = s.SessionId;
                }
                if (targetId == 0 && s.State == WTSDisconnected && s.SessionId != 0) {
                    targetId = s.SessionId;
                }
            }
            WTSFreeMemory(pSessionArray);

            if (targetId == 0) {
                LOG_ERROR("No usable interactive session found");
                throw std::runtime_error("Could not find usable interactive session");
            }
            return targetId;
        }

        // Get string representation of the last error
        std::string SessionHelper::WinApiException::GetLastErrorAsString() {
            DWORD errorCode = GetLastError();

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
            LocalFree(messageBuffer);
            return message;
        }

    }

}