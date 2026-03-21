#include "SessionHelper.h"
#include <sstream>
#include <iostream>
#include <vector>
#include "Utils.h"

namespace hope {

    namespace rtc {

        // Check if the current process is in the active terminal session
        bool SessionHelper::CheckActiveTerminalSession() {
            LOG_INFO("CheckActiveTerminalSession: Starting to check current process session");

            DWORD currentSessionId = 0;
            DWORD processId = GetCurrentProcessId();

            LOG_DEBUG("CheckActiveTerminalSession: Current process ID = %d", processId);

            if (!ProcessIdToSessionId(processId, &currentSessionId)) {
                LOG_ERROR("CheckActiveTerminalSession: ProcessIdToSessionId failed");
                throw WinApiException("ProcessIdToSessionId");
            }

            LOG_DEBUG("CheckActiveTerminalSession: Current session ID = %d", currentSessionId);

            DWORD activeSessionId = GetActiveTerminalSessionId();
            LOG_DEBUG("CheckActiveTerminalSession: Active session ID = %d", activeSessionId);

            bool result = currentSessionId == activeSessionId;
            LOG_INFO("CheckActiveTerminalSession: Result = %s", result ? "true" : "false");

            return result;
        }

        // Create a System-level process in the user session, supporting WGC
        HANDLE SessionHelper::CreateSystemProcessInUserSession(const std::wstring& args) {
            LOG_INFO("CreateSystemProcessInUserSession: Starting to create System-level process, args length=%zu", args.length());

            HANDLE systemToken = nullptr;
            HANDLE duplicatedToken = nullptr;
            LPVOID pEnv = nullptr;

            try {
                // Step 1: Get System Token
                LOG_DEBUG("CreateSystemProcessInUserSession: Attempting to get System Token");
                systemToken = GetSystemToken();

                if (!systemToken) {
                    LOG_ERROR("CreateSystemProcessInUserSession: Failed to get System Token");
                    throw std::runtime_error("Failed to get System Token");
                }
                LOG_INFO("CreateSystemProcessInUserSession: Successfully got System Token");

                // Step 2: Get active user session ID
                DWORD sessionId = GetActiveTerminalSessionId();
                LOG_INFO("CreateSystemProcessInUserSession: Target session ID = %d", sessionId);

                // Step 3: Duplicate Token and set session ID
                LOG_DEBUG("CreateSystemProcessInUserSession: Duplicating System Token");
                if (!DuplicateTokenEx(systemToken, MAXIMUM_ALLOWED, nullptr,
                    SecurityIdentification, TokenPrimary, &duplicatedToken)) {
                    LOG_ERROR("CreateSystemProcessInUserSession: DuplicateTokenEx failed");
                    throw WinApiException("DuplicateTokenEx");
                }
                LOG_DEBUG("CreateSystemProcessInUserSession: DuplicateTokenEx succeeded");

                // Set Token session information
                LOG_DEBUG("CreateSystemProcessInUserSession: Setting Token session information");
                if (!SetTokenInformation(duplicatedToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
                    LOG_ERROR("CreateSystemProcessInUserSession: SetTokenInformation failed");
                    throw WinApiException("SetTokenInformation");
                }
                LOG_DEBUG("CreateSystemProcessInUserSession: SetTokenInformation succeeded");

                // Step 4: Enable all privileges for System Token (including those required for WGC)
                LOG_DEBUG("CreateSystemProcessInUserSession: Enabling Token privileges");
                EnableTokenPrivileges(duplicatedToken);

                // Step 5: Create environment block
                LOG_DEBUG("CreateSystemProcessInUserSession: Creating environment block");
                if (!CreateEnvironmentBlock(&pEnv, duplicatedToken, TRUE)) {
                    LOG_WARNING("CreateSystemProcessInUserSession: CreateEnvironmentBlock failed, continuing");
                    pEnv = nullptr;
                }
                else {
                    LOG_DEBUG("CreateSystemProcessInUserSession: CreateEnvironmentBlock succeeded");
                }

                // Step 6: Get current process executable path
                LOG_DEBUG("CreateSystemProcessInUserSession: Getting current executable path");
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("CreateSystemProcessInUserSession: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                std::wstring exePathStr(exePath);
                std::string exePathMB(exePathStr.begin(), exePathStr.end());
                LOG_INFO("CreateSystemProcessInUserSession: Executable path = %s", exePathMB.c_str());

                // Step 7: Build command line
                std::wstring commandLine = std::wstring(exePath);
                if (!args.empty()) {
                    commandLine += L" " + args;
                    std::string argsMB(args.begin(), args.end());
                    LOG_INFO("CreateSystemProcessInUserSession: Command line arguments = %s", argsMB.c_str());
                }

                // Step 8: Set startup information, specify desktop as user desktop
                LOG_DEBUG("CreateSystemProcessInUserSession: Initializing STARTUPINFO");
                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);
                // Important: Specify user desktop to access graphical interface and WGC
                startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

                PROCESS_INFORMATION processInformation = { 0 };

                // Create mutable command line copy
                std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
                cmdLine.push_back(0);

                // Set creation flags
                DWORD creationFlags = 0;  // Create new console
                creationFlags = CREATE_NO_WINDOW;  // Do not create new window
                if (pEnv) {
                    creationFlags |= CREATE_UNICODE_ENVIRONMENT;
                    LOG_DEBUG("CreateSystemProcessInUserSession: Using Unicode environment flag");
                }

                LOG_INFO("CreateSystemProcessInUserSession: Calling CreateProcessAsUserW, creation flags=%d", creationFlags);

                // Step 9: Create process
                if (!CreateProcessAsUserW(
                    duplicatedToken,    // Use duplicated System Token
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

                LOG_INFO("CreateSystemProcessInUserSession: CreateProcessAsUserW succeeded, new process ID=%d", processInformation.dwProcessId);

                // Verify new process privilege level
                VerifyProcessPrivileges(processInformation.dwProcessId);

                // Close process and thread handles
                LOG_DEBUG("CreateSystemProcessInUserSession: Closing process and thread handles");
                HANDLE hProcess = processInformation.hProcess;
                CloseHandle(processInformation.hThread);

                LOG_INFO("CreateSystemProcessInUserSession: Successfully completed");

                return hProcess;
            }
            catch (const std::exception& e) {
                LOG_ERROR("CreateSystemProcessInUserSession: Caught exception - %s", e.what());
                // Clean up resources
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (duplicatedToken) CloseHandle(duplicatedToken);
                if (systemToken) CloseHandle(systemToken);
                throw;
            }
            catch (...) {
                LOG_ERROR("CreateSystemProcessInUserSession: Caught unknown exception");
                // Clean up resources
                if (pEnv) DestroyEnvironmentBlock(pEnv);
                if (duplicatedToken) CloseHandle(duplicatedToken);
                if (systemToken) CloseHandle(systemToken);
                throw;
            }

            // Clean up resources
            if (pEnv) {
                LOG_DEBUG("CreateSystemProcessInUserSession: Destroying environment block");
                DestroyEnvironmentBlock(pEnv);
            }
            if (duplicatedToken) {
                LOG_DEBUG("CreateSystemProcessInUserSession: Closing duplicatedToken handle");
                CloseHandle(duplicatedToken);
            }
            if (systemToken) {
                LOG_DEBUG("CreateSystemProcessInUserSession: Closing systemToken handle");
                CloseHandle(systemToken);
            }

            return nullptr; // Return nullptr if exception occurs
        }

        // Get System Token
        HANDLE SessionHelper::GetSystemToken() {
            LOG_INFO("GetSystemToken: Starting to get System Token");

            HANDLE processHandle = nullptr;
            HANDLE tokenHandle = nullptr;

            try {
                // Method 1: Try to get from current process (if current process has system privileges)
                if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tokenHandle)) {
                    if (IsSystemToken(tokenHandle)) {
                        LOG_INFO("GetSystemToken: Current process already has System privileges");
                        return tokenHandle;
                    }
                    CloseHandle(tokenHandle);
                    tokenHandle = nullptr;
                }

                // Method 2: Try to get Token from System process
                LOG_DEBUG("GetSystemToken: Attempting to get Token from System process");

                // Find System process (usually ID 4)
                DWORD systemProcessId = FindSystemProcessId();
                if (systemProcessId == 0) {
                    LOG_ERROR("GetSystemToken: Failed to find System process");
                    throw std::runtime_error("Cannot find System process");
                }

                LOG_DEBUG("GetSystemToken: System process ID = %d", systemProcessId);

                // Open System process
                processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, systemProcessId);
                if (!processHandle) {
                    LOG_ERROR("GetSystemToken: Failed to open System process");
                    throw WinApiException("OpenProcess");
                }

                // Get System process Token
                if (!OpenProcessToken(processHandle, TOKEN_DUPLICATE | TOKEN_QUERY, &tokenHandle)) {
                    LOG_ERROR("GetSystemToken: Failed to get System process Token");
                    throw WinApiException("OpenProcessToken");
                }

                LOG_INFO("GetSystemToken: Successfully got System Token");
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
            LOG_DEBUG("FindSystemProcessId: Starting to find System process");

            // System process is usually ID 4, but check other possible system processes
            std::vector<DWORD> candidateIds = { 4, 0 }; // System process is usually ID 4

            for (DWORD pid : candidateIds) {
                if (pid == 0) continue;

                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (hProcess) {
                    HANDLE hToken = nullptr;
                    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                        if (IsSystemToken(hToken)) {
                            LOG_INFO("FindSystemProcessId: Found System process, ID = %d", pid);
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
                // Get Token user information
                GetTokenInformation(token, TokenUser, nullptr, 0, &returnLength);
                tokenUser = (TOKEN_USER*)malloc(returnLength);

                if (!GetTokenInformation(token, TokenUser, tokenUser, returnLength, &returnLength)) {
                    free(tokenUser);
                    return false;
                }

                // Check if it is SYSTEM SID
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

        // Enable all necessary privileges for Token (including those required for WGC)
        void SessionHelper::EnableTokenPrivileges(HANDLE token) {
            LOG_DEBUG("EnableTokenPrivileges: Starting to enable Token privileges");

            // Privileges required for WGC and graphical access
            std::vector<std::wstring> privileges = {
                SE_DEBUG_NAME,                  // Debug privilege
                SE_TCB_NAME,                   // Trusted computing base privilege
                SE_ASSIGNPRIMARYTOKEN_NAME,    // Assign primary token privilege
                SE_IMPERSONATE_NAME,           // Impersonate privilege
                SE_INCREASE_QUOTA_NAME,        // Increase quota privilege
                SE_CHANGE_NOTIFY_NAME,         // Change notify privilege
                SE_SECURITY_NAME,              // Security privilege
                SE_TAKE_OWNERSHIP_NAME,        // Take ownership privilege
                SE_LOAD_DRIVER_NAME,           // Load driver privilege
                SE_SYSTEM_PROFILE_NAME,        // System profile privilege
                SE_SYSTEMTIME_NAME,            // System time privilege
                SE_PROF_SINGLE_PROCESS_NAME,   // Single process profile privilege
                SE_INC_BASE_PRIORITY_NAME,     // Increase base priority privilege
                SE_CREATE_PAGEFILE_NAME,       // Create pagefile privilege
                SE_CREATE_PERMANENT_NAME,      // Create permanent object privilege
                SE_BACKUP_NAME,                // Backup privilege
                SE_RESTORE_NAME,               // Restore privilege
                SE_SHUTDOWN_NAME,              // Shutdown privilege
                SE_AUDIT_NAME,                 // Audit privilege
                SE_SYSTEM_ENVIRONMENT_NAME,    // System environment privilege
                SE_UNDOCK_NAME,                // Undock privilege
                SE_MANAGE_VOLUME_NAME,         // Manage volume privilege
                SE_RELABEL_NAME,               // Relabel privilege
                SE_INC_WORKING_SET_NAME,       // Increase working set privilege
                SE_TIME_ZONE_NAME,             // Time zone privilege
                SE_CREATE_SYMBOLIC_LINK_NAME   // Create symbolic link privilege
            };

            for (const auto& privilege : privileges) {
                EnablePrivilege(token, privilege);
            }

            LOG_INFO("EnableTokenPrivileges: Privileges enabled successfully");
        }

        // Enable a single privilege
        bool SessionHelper::EnablePrivilege(HANDLE token, const std::wstring& privilegeName) {
            TOKEN_PRIVILEGES tp;
            LUID luid;

            if (!LookupPrivilegeValue(nullptr, privilegeName.c_str(), &luid)) {
                LOG_DEBUG("EnablePrivilege: LookupPrivilegeValue failed - %s",
                    std::string(privilegeName.begin(), privilegeName.end()).c_str());
                return false;
            }

            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

            if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
                LOG_DEBUG("EnablePrivilege: AdjustTokenPrivileges failed - %s",
                    std::string(privilegeName.begin(), privilegeName.end()).c_str());
                return false;
            }

            if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                LOG_DEBUG("EnablePrivilege: Privilege not assigned - %s",
                    std::string(privilegeName.begin(), privilegeName.end()).c_str());
                return false;
            }

            LOG_DEBUG("EnablePrivilege: Successfully enabled privilege - %s",
                std::string(privilegeName.begin(), privilegeName.end()).c_str());
            return true;
        }

        // Verify process privilege level
        void SessionHelper::VerifyProcessPrivileges(DWORD processId) {
            LOG_DEBUG("VerifyProcessPrivileges: Verifying process privileges, PID = %d", processId);

            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
            if (!hProcess) {
                LOG_WARNING("VerifyProcessPrivileges: Failed to open process");
                return;
            }

            HANDLE hToken = nullptr;
            if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                if (IsSystemToken(hToken)) {
                    LOG_INFO("VerifyProcessPrivileges: Confirmed process has System privileges");
                }
                else {
                    LOG_WARNING("VerifyProcessPrivileges: Process does not have System privileges");
                }
                CloseHandle(hToken);
            }

            CloseHandle(hProcess);
        }

        void  SessionHelper::RespawnInActiveTerminalSessionWithArgs(const std::wstring& args) {
            LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Starting to respawn process, args length=%zu", args.length());

            HANDLE token = nullptr;
            HANDLE newToken = nullptr;
            LPVOID pEnv = nullptr;

            try {
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Getting active terminal session ID");
                DWORD sessionId = GetActiveTerminalSessionId();
                LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Target session ID = %d", sessionId);

                // Get the user token for the active session
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Attempting to get user token");
                if (!WTSQueryUserToken(sessionId, &token)) {
                    LOG_WARNING("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken failed, falling back to current process token");

                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Opening current process token");
                    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken failed");
                        throw WinApiException("OpenProcessToken");
                    }
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken succeeded");

                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Duplicating token");
                    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                        SecurityIdentification,
                        TokenPrimary, &newToken)) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx failed");
                        throw WinApiException("DuplicateTokenEx");
                    }
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx succeeded");

                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Setting token session information");
                    if (!SetTokenInformation(newToken, TokenSessionId,
                        &sessionId, sizeof(sessionId))) {
                        LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation failed");
                        throw WinApiException("SetTokenInformation");
                    }
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation succeeded");
                }
                else {
                    LOG_INFO("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken succeeded, using user token directly");
                    newToken = token;
                    token = nullptr;  // Prevent double-close
                }

                // Create environment block for the user context
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Creating environment block");
                if (!CreateEnvironmentBlock(&pEnv, newToken, TRUE)) {
                    LOG_WARNING("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock failed, continuing");
                }
                else {
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock succeeded");
                }

                // Get current process executable path
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Getting current executable path");
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                std::wstring exePathStr(exePath);
                std::string exePathMB(exePathStr.begin(), exePathStr.end());
                LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Executable path = %s", exePathMB.c_str());

                // Build command line with arguments
                std::wstring commandLine = std::wstring(exePath);
                if (!args.empty()) {
                    commandLine += L" " + args;
                    std::string argsMB(args.begin(), args.end());
                    LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Command line arguments = %s", argsMB.c_str());
                }

                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Initializing STARTUPINFO");
                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);
                startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

                PROCESS_INFORMATION processInformation = { 0 };

                // Create mutable copy of command line
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Preparing command line arguments");
                std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
                cmdLine.push_back(0);

                // Use CREATE_UNICODE_ENVIRONMENT flag when we have an environment block
                DWORD creationFlags = 0;
                creationFlags = CREATE_NO_WINDOW;  // Do not create new window
                if (pEnv) {
                    creationFlags |= CREATE_UNICODE_ENVIRONMENT;
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Using Unicode environment flag");
                }

                LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Calling CreateProcessAsUserW, creation flags=%d", creationFlags);
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
                    LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW failed");
                    throw WinApiException("CreateProcessAsUserW");
                }

                LOG_INFO("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW succeeded, new process ID=%d", processInformation.dwProcessId);

                // Close process and thread handles
                LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Closing process and thread handles");

                CloseHandle(processInformation.hProcess);

                CloseHandle(processInformation.hThread);

                // Clean up environment block
                if (pEnv) {
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Destroying environment block");
                    DestroyEnvironmentBlock(pEnv);
                }

                LOG_INFO("RespawnInActiveTerminalSessionWithArgs: Successfully completed");
            }
            catch (const std::exception& e) {
                LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: Caught exception - %s", e.what());

                if (pEnv) {
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Exception handling - Destroying environment block");
                    DestroyEnvironmentBlock(pEnv);
                }
                if (token != nullptr) {
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Exception handling - Closing token handle");
                    CloseHandle(token);
                }
                if (newToken != nullptr && newToken != token) {
                    LOG_DEBUG("RespawnInActiveTerminalSessionWithArgs: Exception handling - Closing newToken handle");
                    CloseHandle(newToken);
                }
                throw;
            }
            catch (...) {
                LOG_ERROR("RespawnInActiveTerminalSessionWithArgs: Caught unknown exception");

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

        // Respawn the current application in the active terminal session (no arguments)
        void SessionHelper::RespawnInActiveTerminalSession() {
            LOG_INFO("RespawnInActiveTerminalSession: Starting to respawn process (no arguments)");

            HANDLE token = nullptr;
            HANDLE newToken = nullptr;

            try {
                LOG_DEBUG("RespawnInActiveTerminalSession: Getting active terminal session ID");
                DWORD sessionId = GetActiveTerminalSessionId();
                LOG_INFO("RespawnInActiveTerminalSession: Target session ID = %d", sessionId);

                LOG_DEBUG("RespawnInActiveTerminalSession: Opening current process token");
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                    LOG_ERROR("RespawnInActiveTerminalSession: OpenProcessToken failed");
                    throw WinApiException("OpenProcessToken");
                }
                LOG_DEBUG("RespawnInActiveTerminalSession: OpenProcessToken succeeded");

                LOG_DEBUG("RespawnInActiveTerminalSession: Duplicating token");
                if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                    SecurityIdentification,
                    TokenPrimary, &newToken)) {
                    LOG_ERROR("RespawnInActiveTerminalSession: DuplicateTokenEx failed");
                    throw WinApiException("DuplicateTokenEx");
                }
                LOG_DEBUG("RespawnInActiveTerminalSession: DuplicateTokenEx succeeded");

                LOG_DEBUG("RespawnInActiveTerminalSession: Setting token session information");
                if (!SetTokenInformation(newToken, TokenSessionId,
                    &sessionId, sizeof(sessionId))) {
                    LOG_ERROR("RespawnInActiveTerminalSession: SetTokenInformation failed");
                    throw WinApiException("SetTokenInformation");
                }
                LOG_DEBUG("RespawnInActiveTerminalSession: SetTokenInformation succeeded");

                // Get current process executable path
                LOG_DEBUG("RespawnInActiveTerminalSession: Getting current executable path");
                wchar_t exePath[MAX_PATH];
                if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
                    LOG_ERROR("RespawnInActiveTerminalSession: GetModuleFileName failed");
                    throw WinApiException("GetModuleFileName");
                }

                std::wstring exePathStr(exePath);
                std::string exePathMB(exePathStr.begin(), exePathStr.end());
                LOG_INFO("RespawnInActiveTerminalSession: Executable path = %s", exePathMB.c_str());

                LOG_DEBUG("RespawnInActiveTerminalSession: Initializing STARTUPINFO");
                STARTUPINFOW startupInfo = { 0 };
                startupInfo.cb = sizeof(startupInfo);

                DWORD creationFlags = 0;
                creationFlags = CREATE_NO_WINDOW;  // Do not create new window


                PROCESS_INFORMATION processInformation = { 0 };

                LOG_INFO("RespawnInActiveTerminalSession: Calling CreateProcessAsUserW");
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
                    LOG_ERROR("RespawnInActiveTerminalSession: CreateProcessAsUserW failed");
                    throw WinApiException("CreateProcessAsUserW");
                }

                LOG_INFO("RespawnInActiveTerminalSession: CreateProcessAsUserW succeeded, new process ID=%d", processInformation.dwProcessId);

                // Close process and thread handles
                LOG_DEBUG("RespawnInActiveTerminalSession: Closing process and thread handles");
                CloseHandle(processInformation.hProcess);
                CloseHandle(processInformation.hThread);

                LOG_INFO("RespawnInActiveTerminalSession: Successfully completed");
            }
            catch (const std::exception& e) {
                LOG_ERROR("RespawnInActiveTerminalSession: Caught exception - %s", e.what());

                if (token != nullptr) {
                    LOG_DEBUG("RespawnInActiveTerminalSession: Exception handling - Closing token handle");
                    CloseHandle(token);
                }
                if (newToken != nullptr) {
                    LOG_DEBUG("RespawnInActiveTerminalSession: Exception handling - Closing newToken handle");
                    CloseHandle(newToken);
                }
                throw;
            }
            catch (...) {
                LOG_ERROR("RespawnInActiveTerminalSession: Caught unknown exception");

                if (token != nullptr) {
                    CloseHandle(token);
                }
                if (newToken != nullptr) {
                    CloseHandle(newToken);
                }
                throw;
            }
        }

        // Get active terminal session ID
        DWORD SessionHelper::GetActiveTerminalSessionId() {
            LOG_INFO("GetActiveTerminalSessionId: Starting to get usable interactive session");

            PWTS_SESSION_INFO pSessionArray = nullptr;
            DWORD sessionCount = 0;

            if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionArray, &sessionCount)) {
                LOG_ERROR("WTSEnumerateSessions failed");
                throw WinApiException("WTSEnumerateSessions");
            }

            DWORD targetId = 0;
            for (DWORD i = 0; i < sessionCount; ++i) {
                const auto& s = pSessionArray[i];
                LOG_DEBUG("Session %lu  ID=%lu  State=%lu",
                    i, s.SessionId, s.State);

                // �� ���������� Active
                if (s.State == WTSActive) {
                    targetId = s.SessionId;
                    break;
                }
                // �� ���� Connected �� RDP
                if (s.State == WTSConnected && s.SessionId != 0) {
                    targetId = s.SessionId;
                    // �� break��������ܻ��� Active
                }
                // �� ��󶵵ף�Disconnected ������ Session 0��Console��
                if (targetId == 0 &&
                    s.State == WTSDisconnected &&
                    s.SessionId != 0)
                    targetId = s.SessionId;
            }
            WTSFreeMemory(pSessionArray);

            if (targetId == 0) {
                LOG_ERROR("No usable interactive session found");
                throw std::runtime_error("Could not find usable interactive session");
            }
            LOG_INFO("Selected session ID = %lu", targetId);
            return targetId;
        }

        // Get string representation of the last error
        std::string SessionHelper::WinApiException::GetLastErrorAsString() {
            DWORD errorCode = GetLastError();
            LOG_DEBUG("WinApiException::GetLastErrorAsString: Error code = %d", errorCode);

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
            LOG_DEBUG("WinApiException::GetLastErrorAsString: Error message = %s", message.c_str());

            LocalFree(messageBuffer);
            return message;
        }

    }

}