#include "StartupManager.h"
#include <windows.h>
#include <shlobj.h>
#include <string>

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_VAL = L"AlienCockpit";
static const wchar_t* TASK_NAME = L"AlienCockpit";

static bool RunHiddenCommand(const std::wstring& cmdLine, DWORD* outExitCode = nullptr) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine;
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (outExitCode) *outExitCode = exitCode;
    return exitCode == 0;
}

static bool IsScheduledTaskPresent() {
    DWORD exitCode = 1;
    std::wstring cmd = std::wstring(L"schtasks.exe /Query /TN \"") + TASK_NAME + L"\"";
    return RunHiddenCommand(cmd, &exitCode);
}

bool StartupManager::IsStartupEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0, size = 0;
        bool exists = (RegQueryValueExW(hKey, APP_VAL, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        if (exists) return true;
    }
    return IsScheduledTaskPresent();
}

bool StartupManager::SetStartupEnabled(bool enable, bool minimized, bool asAdmin) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring appCmd = std::wstring(L"\"") + exePath + L"\"";
    if (minimized) appCmd += L" --minimized";

    // Always clean up existing Run key first
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, APP_VAL);
        RegCloseKey(hKey);
    }

    if (!enable) {
        std::wstring delCmd = std::wstring(L"schtasks.exe /Delete /TN \"") + TASK_NAME + L"\" /F";
        RunHiddenCommand(delCmd);
        return true;
    }

    if (asAdmin && IsUserAnAdmin()) {
        std::wstring trVal = std::wstring(L"\\\"") + exePath + L"\\\"";
        if (minimized) trVal += L" --minimized";
        std::wstring createCmd = std::wstring(L"schtasks.exe /Create /TN \"") + TASK_NAME +
            L"\" /TR \"" + trVal + L"\" /SC ONLOGON /RL HIGHEST /F";
        if (RunHiddenCommand(createCmd)) {
            return true;
        }
    } else {
        std::wstring delCmd = std::wstring(L"schtasks.exe /Delete /TN \"") + TASK_NAME + L"\" /F";
        RunHiddenCommand(delCmd);
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    LSTATUS res = RegSetValueExW(hKey, APP_VAL, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(appCmd.c_str()),
        static_cast<DWORD>((appCmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}
