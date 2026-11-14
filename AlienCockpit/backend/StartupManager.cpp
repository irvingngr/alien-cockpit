#include "StartupManager.h"
#include <windows.h>
#include <string>

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_VAL = L"AlienCockpit";

bool StartupManager::IsStartupEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(hKey, APP_VAL, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

bool StartupManager::SetStartupEnabled(bool enable, bool minimized) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    LSTATUS res = ERROR_SUCCESS;
    if (enable) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = L"\"";
        cmd += exePath;
        cmd += L"\"";
        if (minimized) cmd += L" --minimized";

        res = RegSetValueExW(hKey, APP_VAL, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        res = RegDeleteValueW(hKey, APP_VAL);
        if (res == ERROR_FILE_NOT_FOUND) res = ERROR_SUCCESS;
    }

    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}
