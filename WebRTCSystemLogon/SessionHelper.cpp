#include "SessionHelper.h"
#include <sstream>
#include <iostream>
#include <vector>
#include "Logger.h"

// Check if the current process is in the active terminal session
bool SessionHelper::CheckActiveTerminalSession() {
    Logger::getInstance()->info("CheckActiveTerminalSession: Starting to check current process session");

    DWORD currentSessionId = 0;
    DWORD processId = GetCurrentProcessId();

    Logger::getInstance()->debug("CheckActiveTerminalSession: Current process ID = " + std::to_string(processId));

    if (!ProcessIdToSessionId(processId, &currentSessionId)) {
        Logger::getInstance()->error("CheckActiveTerminalSession: ProcessIdToSessionId failed");
        throw WinApiException("ProcessIdToSessionId");
    }

    Logger::getInstance()->debug("CheckActiveTerminalSession: Current session ID = " + std::to_string(currentSessionId));

    DWORD activeSessionId = GetActiveTerminalSessionId();
    Logger::getInstance()->debug("CheckActiveTerminalSession: Active session ID = " + std::to_string(activeSessionId));

    bool result = currentSessionId == activeSessionId;
    Logger::getInstance()->info("CheckActiveTerminalSession: Result = " + std::string(result ? "true" : "false"));

    return result;
}

// Create a System-level process in the user session, supporting WGC
HANDLE SessionHelper::CreateSystemProcessInUserSession(const std::wstring& args) {
    Logger::getInstance()->info("CreateSystemProcessInUserSession: Starting to create System-level process, args length=" + std::to_string(args.length()));

    HANDLE systemToken = nullptr;
    HANDLE duplicatedToken = nullptr;
    LPVOID pEnv = nullptr;

    try {
        // Step 1: Get System Token
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Attempting to get System Token");
        systemToken = GetSystemToken();

        if (!systemToken) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: Failed to get System Token");
            throw std::runtime_error("Failed to get System Token");
        }
        Logger::getInstance()->info("CreateSystemProcessInUserSession: Successfully got System Token");

        // Step 2: Get active user session ID
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("CreateSystemProcessInUserSession: Target session ID = " + std::to_string(sessionId));

        // Step 3: Duplicate Token and set session ID
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Duplicating System Token");
        if (!DuplicateTokenEx(systemToken, MAXIMUM_ALLOWED, nullptr,
            SecurityIdentification, TokenPrimary, &duplicatedToken)) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: DuplicateTokenEx failed");
            throw WinApiException("DuplicateTokenEx");
        }
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: DuplicateTokenEx succeeded");

        // Set Token session information
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Setting Token session information");
        if (!SetTokenInformation(duplicatedToken, TokenSessionId, &sessionId, sizeof(sessionId))) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: SetTokenInformation failed");
            throw WinApiException("SetTokenInformation");
        }
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: SetTokenInformation succeeded");

        // Step 4: Enable all privileges for System Token (including those required for WGC)
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Enabling Token privileges");
        EnableTokenPrivileges(duplicatedToken);

        // Step 5: Create environment block
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Creating environment block");
        if (!CreateEnvironmentBlock(&pEnv, duplicatedToken, TRUE)) {
            Logger::getInstance()->warning("CreateSystemProcessInUserSession: CreateEnvironmentBlock failed, continuing");
            pEnv = nullptr;
        }
        else {
            Logger::getInstance()->debug("CreateSystemProcessInUserSession: CreateEnvironmentBlock succeeded");
        }

        // Step 6: Get current process executable path
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Getting current executable path");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("CreateSystemProcessInUserSession: GetModuleFileName failed");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("CreateSystemProcessInUserSession: Executable path = " + exePathMB);

        // Step 7: Build command line
        std::wstring commandLine = std::wstring(exePath);
        if (!args.empty()) {
            commandLine += L" " + args;
            std::string argsMB(args.begin(), args.end());
            Logger::getInstance()->info("CreateSystemProcessInUserSession: Command line arguments = " + argsMB);
        }

        // Step 8: Set startup information, specify desktop as user desktop
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Initializing STARTUPINFO");
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
            Logger::getInstance()->debug("CreateSystemProcessInUserSession: Using Unicode environment flag");
        }

        Logger::getInstance()->info("CreateSystemProcessInUserSession: Calling CreateProcessAsUserW, creation flags=" + std::to_string(creationFlags));

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
            Logger::getInstance()->error("CreateSystemProcessInUserSession: CreateProcessAsUserW failed");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("CreateSystemProcessInUserSession: CreateProcessAsUserW succeeded, new process ID=" + std::to_string(processInformation.dwProcessId));

        // Verify new process privilege level
        VerifyProcessPrivileges(processInformation.dwProcessId);

        // Close process and thread handles
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Closing process and thread handles");
        HANDLE hProcess = processInformation.hProcess;
        CloseHandle(processInformation.hThread);

        Logger::getInstance()->info("CreateSystemProcessInUserSession: Successfully completed");

        return hProcess;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("CreateSystemProcessInUserSession: Caught exception - " + std::string(e.what()));
        // Clean up resources
        if (pEnv) DestroyEnvironmentBlock(pEnv);
        if (duplicatedToken) CloseHandle(duplicatedToken);
        if (systemToken) CloseHandle(systemToken);
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("CreateSystemProcessInUserSession: Caught unknown exception");
        // Clean up resources
        if (pEnv) DestroyEnvironmentBlock(pEnv);
        if (duplicatedToken) CloseHandle(duplicatedToken);
        if (systemToken) CloseHandle(systemToken);
        throw;
    }

    // Clean up resources
    if (pEnv) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Destroying environment block");
        DestroyEnvironmentBlock(pEnv);
    }
    if (duplicatedToken) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Closing duplicatedToken handle");
        CloseHandle(duplicatedToken);
    }
    if (systemToken) {
        Logger::getInstance()->debug("CreateSystemProcessInUserSession: Closing systemToken handle");
        CloseHandle(systemToken);
    }

    return nullptr; // Return nullptr if exception occurs
}

// Get System Token
HANDLE SessionHelper::GetSystemToken() {
    Logger::getInstance()->info("GetSystemToken: Starting to get System Token");

    HANDLE processHandle = nullptr;
    HANDLE tokenHandle = nullptr;

    try {
        // Method 1: Try to get from current process (if current process has system privileges)
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &tokenHandle)) {
            if (IsSystemToken(tokenHandle)) {
                Logger::getInstance()->info("GetSystemToken: Current process already has System privileges");
                return tokenHandle;
            }
            CloseHandle(tokenHandle);
            tokenHandle = nullptr;
        }

        // Method 2: Try to get Token from System process
        Logger::getInstance()->debug("GetSystemToken: Attempting to get Token from System process");

        // Find System process (usually ID 4)
        DWORD systemProcessId = FindSystemProcessId();
        if (systemProcessId == 0) {
            Logger::getInstance()->error("GetSystemToken: Failed to find System process");
            throw std::runtime_error("Cannot find System process");
        }

        Logger::getInstance()->debug("GetSystemToken: System process ID = " + std::to_string(systemProcessId));

        // Open System process
        processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, systemProcessId);
        if (!processHandle) {
            Logger::getInstance()->error("GetSystemToken: Failed to open System process");
            throw WinApiException("OpenProcess");
        }

        // Get System process Token
        if (!OpenProcessToken(processHandle, TOKEN_DUPLICATE | TOKEN_QUERY, &tokenHandle)) {
            Logger::getInstance()->error("GetSystemToken: Failed to get System process Token");
            throw WinApiException("OpenProcessToken");
        }

        Logger::getInstance()->info("GetSystemToken: Successfully got System Token");
        CloseHandle(processHandle);
        return tokenHandle;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("GetSystemToken: Exception - " + std::string(e.what()));
        if (processHandle) CloseHandle(processHandle);
        if (tokenHandle) CloseHandle(tokenHandle);
        throw;
    }
}

// Find System process ID
DWORD SessionHelper::FindSystemProcessId() {
    Logger::getInstance()->debug("FindSystemProcessId: Starting to find System process");

    // System process is usually ID 4, but check other possible system processes
    std::vector<DWORD> candidateIds = { 4, 0 }; // System process is usually ID 4

    for (DWORD pid : candidateIds) {
        if (pid == 0) continue;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (hProcess) {
            HANDLE hToken = nullptr;
            if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
                if (IsSystemToken(hToken)) {
                    Logger::getInstance()->info("FindSystemProcessId: Found System process, ID = " + std::to_string(pid));
                    CloseHandle(hToken);
                    CloseHandle(hProcess);
                    return pid;
                }
                CloseHandle(hToken);
            }
            CloseHandle(hProcess);
        }
    }

    Logger::getInstance()->warning("FindSystemProcessId: System process not found");
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
    Logger::getInstance()->debug("EnableTokenPrivileges: Starting to enable Token privileges");

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

    Logger::getInstance()->info("EnableTokenPrivileges: Privileges enabled successfully");
}

// Enable a single privilege
bool SessionHelper::EnablePrivilege(HANDLE token, const std::wstring& privilegeName) {
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValue(nullptr, privilegeName.c_str(), &luid)) {
        Logger::getInstance()->debug("EnablePrivilege: LookupPrivilegeValue failed - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr)) {
        Logger::getInstance()->debug("EnablePrivilege: AdjustTokenPrivileges failed - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        Logger::getInstance()->debug("EnablePrivilege: Privilege not assigned - " +
            std::string(privilegeName.begin(), privilegeName.end()));
        return false;
    }

    Logger::getInstance()->debug("EnablePrivilege: Successfully enabled privilege - " +
        std::string(privilegeName.begin(), privilegeName.end()));
    return true;
}

// Verify process privilege level
void SessionHelper::VerifyProcessPrivileges(DWORD processId) {
    Logger::getInstance()->debug("VerifyProcessPrivileges: Verifying process privileges, PID = " + std::to_string(processId));

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (!hProcess) {
        Logger::getInstance()->warning("VerifyProcessPrivileges: Failed to open process");
        return;
    }

    HANDLE hToken = nullptr;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
        if (IsSystemToken(hToken)) {
            Logger::getInstance()->info("VerifyProcessPrivileges: Confirmed process has System privileges");
        }
        else {
            Logger::getInstance()->warning("VerifyProcessPrivileges: Process does not have System privileges");
        }
        CloseHandle(hToken);
    }

    CloseHandle(hProcess);
}

void  SessionHelper::RespawnInActiveTerminalSessionWithArgs(const std::wstring& args) {
    Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Starting to respawn process, args length=" + std::to_string(args.length()));

    HANDLE token = nullptr;
    HANDLE newToken = nullptr;
    LPVOID pEnv = nullptr;

    try {
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Getting active terminal session ID");
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Target session ID = " + std::to_string(sessionId));

        // Get the user token for the active session
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Attempting to get user token");
        if (!WTSQueryUserToken(sessionId, &token)) {
            Logger::getInstance()->warning("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken failed, falling back to current process token");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Opening current process token");
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken failed");
                throw WinApiException("OpenProcessToken");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: OpenProcessToken succeeded");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Duplicating token");
            if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
                SecurityIdentification,
                TokenPrimary, &newToken)) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx failed");
                throw WinApiException("DuplicateTokenEx");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: DuplicateTokenEx succeeded");

            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Setting token session information");
            if (!SetTokenInformation(newToken, TokenSessionId,
                &sessionId, sizeof(sessionId))) {
                Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation failed");
                throw WinApiException("SetTokenInformation");
            }
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: SetTokenInformation succeeded");
        }
        else {
            Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: WTSQueryUserToken succeeded, using user token directly");
            newToken = token;
            token = nullptr;  // Prevent double-close
        }

        // Create environment block for the user context
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Creating environment block");
        if (!CreateEnvironmentBlock(&pEnv, newToken, TRUE)) {
            Logger::getInstance()->warning("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock failed, continuing");
        }
        else {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: CreateEnvironmentBlock succeeded");
        }

        // Get current process executable path
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Getting current executable path");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: GetModuleFileName failed");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Executable path = " + exePathMB);

        // Build command line with arguments
        std::wstring commandLine = std::wstring(exePath);
        if (!args.empty()) {
            commandLine += L" " + args;
            std::string argsMB(args.begin(), args.end());
            Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Command line arguments = " + argsMB);
        }

        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Initializing STARTUPINFO");
        STARTUPINFOW startupInfo = { 0 };
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

        PROCESS_INFORMATION processInformation = { 0 };

        // Create mutable copy of command line
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Preparing command line arguments");
        std::vector<wchar_t> cmdLine(commandLine.begin(), commandLine.end());
        cmdLine.push_back(0);

        // Use CREATE_UNICODE_ENVIRONMENT flag when we have an environment block
        DWORD creationFlags = 0;
        creationFlags = CREATE_NO_WINDOW;  // Do not create new window
        if (pEnv) {
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Using Unicode environment flag");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Calling CreateProcessAsUserW, creation flags=" + std::to_string(creationFlags));
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
            Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW failed");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: CreateProcessAsUserW succeeded, new process ID=" + std::to_string(processInformation.dwProcessId));

        // Close process and thread handles
        Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Closing process and thread handles");

        CloseHandle(processInformation.hProcess);

        CloseHandle(processInformation.hThread);

        // Clean up environment block
        if (pEnv) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Destroying environment block");
            DestroyEnvironmentBlock(pEnv);
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSessionWithArgs: Successfully completed");
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: Caught exception - " + std::string(e.what()));

        if (pEnv) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Exception handling - Destroying environment block");
            DestroyEnvironmentBlock(pEnv);
        }
        if (token != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Exception handling - Closing token handle");
            CloseHandle(token);
        }
        if (newToken != nullptr && newToken != token) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSessionWithArgs: Exception handling - Closing newToken handle");
            CloseHandle(newToken);
        }
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("RespawnInActiveTerminalSessionWithArgs: Caught unknown exception");

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
    Logger::getInstance()->info("RespawnInActiveTerminalSession: Starting to respawn process (no arguments)");

    HANDLE token = nullptr;
    HANDLE newToken = nullptr;

    try {
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Getting active terminal session ID");
        DWORD sessionId = GetActiveTerminalSessionId();
        Logger::getInstance()->info("RespawnInActiveTerminalSession: Target session ID = " + std::to_string(sessionId));

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Opening current process token");
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &token)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: OpenProcessToken failed");
            throw WinApiException("OpenProcessToken");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: OpenProcessToken succeeded");

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Duplicating token");
        if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr,
            SecurityIdentification,
            TokenPrimary, &newToken)) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: DuplicateTokenEx failed");
            throw WinApiException("DuplicateTokenEx");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: DuplicateTokenEx succeeded");

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Setting token session information");
        if (!SetTokenInformation(newToken, TokenSessionId,
            &sessionId, sizeof(sessionId))) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: SetTokenInformation failed");
            throw WinApiException("SetTokenInformation");
        }
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: SetTokenInformation succeeded");

        // Get current process executable path
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Getting current executable path");
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileName(nullptr, exePath, MAX_PATH) == 0) {
            Logger::getInstance()->error("RespawnInActiveTerminalSession: GetModuleFileName failed");
            throw WinApiException("GetModuleFileName");
        }

        std::wstring exePathStr(exePath);
        std::string exePathMB(exePathStr.begin(), exePathStr.end());
        Logger::getInstance()->info("RespawnInActiveTerminalSession: Executable path = " + exePathMB);

        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Initializing STARTUPINFO");
        STARTUPINFOW startupInfo = { 0 };
        startupInfo.cb = sizeof(startupInfo);

        DWORD creationFlags = 0;
        creationFlags = CREATE_NO_WINDOW;  // Do not create new window


        PROCESS_INFORMATION processInformation = { 0 };

        Logger::getInstance()->info("RespawnInActiveTerminalSession: Calling CreateProcessAsUserW");
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
            Logger::getInstance()->error("RespawnInActiveTerminalSession: CreateProcessAsUserW failed");
            throw WinApiException("CreateProcessAsUserW");
        }

        Logger::getInstance()->info("RespawnInActiveTerminalSession: CreateProcessAsUserW succeeded, new process ID=" + std::to_string(processInformation.dwProcessId));

        // Close process and thread handles
        Logger::getInstance()->debug("RespawnInActiveTerminalSession: Closing process and thread handles");
        CloseHandle(processInformation.hProcess);
        CloseHandle(processInformation.hThread);

        Logger::getInstance()->info("RespawnInActiveTerminalSession: Successfully completed");
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("RespawnInActiveTerminalSession: Caught exception - " + std::string(e.what()));

        if (token != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSession: Exception handling - Closing token handle");
            CloseHandle(token);
        }
        if (newToken != nullptr) {
            Logger::getInstance()->debug("RespawnInActiveTerminalSession: Exception handling - Closing newToken handle");
            CloseHandle(newToken);
        }
        throw;
    }
    catch (...) {
        Logger::getInstance()->error("RespawnInActiveTerminalSession: Caught unknown exception");

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
    Logger::getInstance()->info("GetActiveTerminalSessionId: Starting to get active terminal session ID");

    PWTS_SESSION_INFO pSessionArray = nullptr;
    DWORD sessionCount = 0;

    try {
        Logger::getInstance()->debug("GetActiveTerminalSessionId: Calling WTSEnumerateSessions");
        if (!WTSEnumerateSessions(nullptr, 0, 1, &pSessionArray, &sessionCount)) {
            Logger::getInstance()->error("GetActiveTerminalSessionId: WTSEnumerateSessions failed");
            throw WinApiException("WTSEnumerateSessions");
        }

        Logger::getInstance()->info("GetActiveTerminalSessionId: Found " + std::to_string(sessionCount) + " sessions");

        DWORD activeSessionId = 0;
        bool sessionFound = false;

        // Iterate through all sessions
        for (DWORD i = 0; i < sessionCount; i++) {
            WTS_SESSION_INFO session = pSessionArray[i];

            Logger::getInstance()->debug("GetActiveTerminalSessionId: Session " + std::to_string(i) +
                " - ID=" + std::to_string(session.SessionId) +
                ", State=" + std::to_string(session.State));

            if (session.State == WTSActive) {
                activeSessionId = session.SessionId;
                sessionFound = true;
                Logger::getInstance()->info("GetActiveTerminalSessionId: Found active session ID = " + std::to_string(activeSessionId));
                break;
            }
        }

        WTSFreeMemory(pSessionArray);

        if (!sessionFound) {
            Logger::getInstance()->error("GetActiveTerminalSessionId: No active terminal session found");
            throw std::runtime_error("Could not find active terminal session.");
        }

        Logger::getInstance()->info("GetActiveTerminalSessionId: Successfully returning session ID = " + std::to_string(activeSessionId));
        return activeSessionId;
    }
    catch (const std::exception& e) {
        Logger::getInstance()->error("GetActiveTerminalSessionId: Caught exception - " + std::string(e.what()));
        if (pSessionArray != nullptr) {
            WTSFreeMemory(pSessionArray);
        }
        throw;
    }
}

// Get string representation of the last error
std::string SessionHelper::WinApiException::GetLastErrorAsString() {
    DWORD errorCode = GetLastError();
    Logger::getInstance()->debug("WinApiException::GetLastErrorAsString: Error code = " + std::to_string(errorCode));

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
    Logger::getInstance()->debug("WinApiException::GetLastErrorAsString: Error message = " + message);

    LocalFree(messageBuffer);
    return message;
}