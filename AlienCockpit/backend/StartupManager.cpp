#include "StartupManager.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <string>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_VAL = L"AlienCockpit";
static const wchar_t* TASK_NAME = L"AlienCockpit";

namespace {

struct ScopedComInit {
    HRESULT hr = E_FAIL;
    bool needUninit = false;
    ScopedComInit() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == S_OK || hr == S_FALSE) {
            needUninit = true;
        }
    }
    ~ScopedComInit() {
        if (needUninit) {
            CoUninitialize();
        }
    }
    bool ok() const {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
};

struct ScopedBstr {
    BSTR bstr = nullptr;
    explicit ScopedBstr(const wchar_t* s) : bstr(s ? SysAllocString(s) : nullptr) {}
    explicit ScopedBstr(const std::wstring& s) : bstr(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~ScopedBstr() {
        if (bstr) SysFreeString(bstr);
    }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
    operator BSTR() const { return bstr; }
};

std::wstring GetCurrentExePath() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::wstring(exePath);
}

std::wstring GetCurrentExeDir() {
    wchar_t dir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    PathRemoveFileSpecW(dir);
    return std::wstring(dir);
}

std::wstring TrimWhitespace(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring ExtractExePathFromCommand(const std::wstring& rawCmd) {
    std::wstring s = TrimWhitespace(rawCmd);
    if (s.empty()) return {};
    if (s.front() == L'"') {
        size_t close = s.find(L'"', 1);
        if (close != std::wstring::npos) {
            return s.substr(1, close - 1);
        }
        return s.substr(1);
    }
    // Unquoted path: handle either no args or args separated by space
    size_t exePos = s.find(L".exe");
    if (exePos == std::wstring::npos) exePos = s.find(L".EXE");
    if (exePos != std::wstring::npos) {
        return s.substr(0, exePos + 4);
    }
    size_t spacePos = s.find_first_of(L" \t");
    return (spacePos == std::wstring::npos) ? s : s.substr(0, spacePos);
}

std::wstring NormalizeFullPath(const std::wstring& p) {
    std::wstring trimmed = TrimWhitespace(p);
    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    if (trimmed.empty()) return {};
    wchar_t fullBuf[MAX_PATH]{};
    DWORD len = GetFullPathNameW(trimmed.c_str(), MAX_PATH, fullBuf, nullptr);
    if (len > 0 && len < MAX_PATH) {
        return std::wstring(fullBuf);
    }
    return trimmed;
}

bool IsSameExePath(const std::wstring& a, const std::wstring& b) {
    std::wstring na = NormalizeFullPath(a);
    std::wstring nb = NormalizeFullPath(b);
    if (na.empty() || nb.empty()) return false;
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

bool GetRunKeyExePath(std::wstring& outPath) {
    outPath.clear();
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    bool found = false;
    if (RegQueryValueExW(hKey, APP_VAL, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= sizeof(wchar_t)) {
        std::wstring buf(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(hKey, APP_VAL, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &bytes) == ERROR_SUCCESS) {
            while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
            outPath = ExtractExePathFromCommand(buf);
            found = !outPath.empty();
        }
    }
    RegCloseKey(hKey);
    return found;
}

bool ConnectTaskRoot(ComPtr<ITaskService>& outService, ComPtr<ITaskFolder>& outRoot) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&outService));
    if (FAILED(hr) || !outService) return false;
    VARIANT empty{};
    VariantInit(&empty);
    hr = outService->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return false;
    ScopedBstr rootPath(L"\\");
    hr = outService->GetFolder(rootPath, &outRoot);
    return SUCCEEDED(hr) && outRoot;
}

bool GetScheduledTaskExePath(std::wstring& outPath, bool* outPresent = nullptr) {
    outPath.clear();
    if (outPresent) *outPresent = false;
    ScopedComInit com;
    if (!com.ok()) return false;

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!ConnectTaskRoot(service, root)) return false;

    ScopedBstr taskName(TASK_NAME);
    ComPtr<IRegisteredTask> task;
    HRESULT hr = root->GetTask(taskName, &task);
    if (FAILED(hr) || !task) {
        return false;
    }
    if (outPresent) *outPresent = true;

    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(&def)) || !def) return false;

    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(&actions)) || !actions) return false;

    ComPtr<IAction> action;
    if (FAILED(actions->get_Item(1, &action)) || !action) return false;

    ComPtr<IExecAction> execAction;
    if (FAILED(action.As(&execAction)) || !execAction) return false;

    BSTR pathBstr = nullptr;
    if (SUCCEEDED(execAction->get_Path(&pathBstr)) && pathBstr) {
        std::wstring raw(pathBstr, SysStringLen(pathBstr));
        SysFreeString(pathBstr);
        outPath = ExtractExePathFromCommand(raw);
        return !outPath.empty();
    }
    return false;
}

bool DeleteScheduledTask(bool allowElevatePrompt) {
    ScopedComInit com;
    if (!com.ok()) return false;

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!ConnectTaskRoot(service, root)) return false;

    ScopedBstr taskName(TASK_NAME);
    HRESULT hr = root->DeleteTask(taskName, 0);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return true;
    }

    if (hr == E_ACCESSDENIED && allowElevatePrompt) {
        std::wstring params = std::wstring(L"/Delete /TN \"") + TASK_NAME + L"\" /F";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"schtasks.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 5000);
                DWORD exitCode = 1;
                GetExitCodeProcess(sei.hProcess, &exitCode);
                CloseHandle(sei.hProcess);
                return exitCode == 0;
            }
            return true;
        }
    }
    return false;
}

bool CreateOrUpdateElevatedScheduledTask(const std::wstring& exePath, const std::wstring& exeDir, bool minimized) {
    ScopedComInit com;
    if (!com.ok()) return false;

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!ConnectTaskRoot(service, root)) return false;

    ComPtr<ITaskDefinition> def;
    if (FAILED(service->NewTask(0, &def)) || !def) return false;

    ComPtr<IRegistrationInfo> regInfo;
    if (SUCCEEDED(def->get_RegistrationInfo(&regInfo)) && regInfo) {
        ScopedBstr author(L"AlienCockpit");
        regInfo->put_Author(author);
    }

    ComPtr<IPrincipal> principal;
    if (FAILED(def->get_Principal(&principal)) || !principal) return false;
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(def->get_Settings(&settings)) && settings) {
        ScopedBstr noLimit(L"PT0S");
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(noLimit);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
    }

    ComPtr<ITriggerCollection> triggers;
    if (FAILED(def->get_Triggers(&triggers)) || !triggers) return false;
    ComPtr<ITrigger> trigger;
    if (FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) || !trigger) return false;
    ComPtr<ILogonTrigger> logonTrigger;
    if (SUCCEEDED(trigger.As(&logonTrigger)) && logonTrigger) {
        ScopedBstr trigId(L"LogonTrigger");
        logonTrigger->put_Id(trigId);
        logonTrigger->put_Enabled(VARIANT_TRUE);
    }

    ComPtr<IActionCollection> actions;
    if (FAILED(def->get_Actions(&actions)) || !actions) return false;
    ComPtr<IAction> action;
    if (FAILED(actions->Create(TASK_ACTION_EXEC, &action)) || !action) return false;
    ComPtr<IExecAction> execAction;
    if (FAILED(action.As(&execAction)) || !execAction) return false;

    ScopedBstr bPath(exePath);
    ScopedBstr bDir(exeDir);
    if (FAILED(execAction->put_Path(bPath))) return false;
    if (minimized) {
        ScopedBstr bArgs(L"--minimized");
        execAction->put_Arguments(bArgs);
    }
    execAction->put_WorkingDirectory(bDir);

    VARIANT empty{};
    VariantInit(&empty);
    ScopedBstr bTaskName(TASK_NAME);
    ComPtr<IRegisteredTask> registered;
    HRESULT hr = root->RegisterTaskDefinition(
        bTaskName,
        def.Get(),
        TASK_CREATE_OR_UPDATE,
        empty,
        empty,
        TASK_LOGON_INTERACTIVE_TOKEN,
        empty,
        &registered);
    return SUCCEEDED(hr);
}

} // namespace

bool StartupManager::IsStartupEnabled() {
    std::wstring currentExe = GetCurrentExePath();

    std::wstring runExe;
    if (GetRunKeyExePath(runExe) && IsSameExePath(runExe, currentExe)) {
        return true;
    }

    std::wstring taskExe;
    if (GetScheduledTaskExePath(taskExe) && IsSameExePath(taskExe, currentExe)) {
        return true;
    }

    return false;
}

bool StartupManager::SetStartupEnabled(bool enable, bool minimized, bool asAdmin) {
    std::wstring exePath = GetCurrentExePath();
    std::wstring exeDir = GetCurrentExeDir();
    std::wstring appCmd = L"\"" + exePath + L"\"";
    if (minimized) appCmd += L" --minimized";

    // Always clean up existing Run key first
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, APP_VAL);
        RegCloseKey(hKey);
    }

    if (!enable) {
        DeleteScheduledTask(true);
        return true;
    }

    if (asAdmin && IsUserAnAdmin()) {
        if (CreateOrUpdateElevatedScheduledTask(exePath, exeDir, minimized)) {
            return true;
        }
    } else {
        DeleteScheduledTask(true);
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    LSTATUS res = RegSetValueExW(hKey, APP_VAL, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(appCmd.c_str()),
        static_cast<DWORD>((appCmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}
