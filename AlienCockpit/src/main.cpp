#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>
#include <vector>

#include "resource.h"
#include "../backend/alienfx-gui.h"
#include "../backend/FXHelper.h"
#include "../backend/EventHandler.h"
#include "../backend/CockpitBridge.h"
#include "../backend/BatteryMonitor.h"
#include "MonHelper.h"
#include "ConfigFan.h"
#include <algorithm>
#include <fstream>
#include <sstream>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;

// Global backend instances expected by extracted engine
HINSTANCE hInst = nullptr;
HWND mDlg = nullptr;
ConfigHandler* conf = nullptr;
FXHelper* fxhl = nullptr;
EventHandler* eve = nullptr;
MonHelper* mon = nullptr;
ConfigFan* fan_conf = nullptr;
HANDLE haveLightFX = nullptr;
groupset* activeMapping = nullptr;
int idc_version = 0;
int idc_homepage = 0;
bool isNewVersion = false;
bool needUpdateFeedback = false;

void Code2Act(AlienFX_SDK::Afx_colorcode c, AlienFX_SDK::Afx_action* fill) {
    if (fill) {
        fill->r = c.r;
        fill->g = c.g;
        fill->b = c.b;
    }
}

static ComPtr<ICoreWebView2Controller> g_webviewController;
static ComPtr<ICoreWebView2> g_webview;
static NOTIFYICONDATAW g_nid{};
static bool g_trayAdded = false;
static HANDLE g_hMutex = nullptr;
static const UINT_PTR TIMER_TELEMETRY = 1;
static const UINT_PTR TIMER_EFFECTS = 2;
static std::vector<BatteryDeviceInfo> g_cachedBatteries;
static ULONGLONG g_lastBatteryScanTick = 0;

static bool RelaunchAsAdmin(HWND hwnd, bool minimized) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring params = L"--relaunch";
    if (minimized) params += L" --minimized";

    if (g_hMutex) {
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params.c_str();
    sei.nShow = minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        if (hwnd) DestroyWindow(hwnd);
        return true;
    }

    // UAC canceled or failed; re-acquire single-instance mutex
    g_hMutex = CreateMutexW(nullptr, TRUE, L"Local\\AlienCockpitSingleInstance");
    return false;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return std::wstring(path);
}

static std::wstring GetWebViewUserDataFolder() {
    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData)) && appData[0] != L'\0') {
        std::wstring folder = std::wstring(appData) + L"\\AlienCockpit\\WebView2Data";
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        return folder;
    }
    return GetExeDirectory() + L"\\WebView2Data";
}

static void UpdateTrayIconAndTooltip(bool forceScan = false) {
    if (!g_trayAdded || !conf) return;
    UINT iconRes = conf->lightsOn ? (conf->stateDimmed ? IDI_ALIENFX_DIM : IDI_ALIENFX_ON) : IDI_ALIENFX_OFF;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(iconRes));

    std::wstring tip = L"AlienCockpit | Lights: ";
    tip += (conf->lightsOn ? L"ON" : L"OFF");

    ULONGLONG now = GetTickCount64();
    if (forceScan || g_lastBatteryScanTick == 0 || (now - g_lastBatteryScanTick) >= 6000) {
        g_cachedBatteries = BatteryMonitor::GetAllBatteries();
        g_lastBatteryScanTick = now;
    }

    for (const auto& b : g_cachedBatteries) {
        std::wstring part;
        if (b.type == "system") {
            part = L" | Laptop: " + std::to_wstring(b.percentage) + L"%";
        } else if (b.type == "mouse") {
            part = L" | Mouse: " + std::to_wstring(b.percentage) + L"%";
        } else {
            part = L" | " + Utf8ToWide(b.name.substr(0, 14)) + L": " + std::to_wstring(b.percentage) + L"%";
        }
        if (tip.size() + part.size() < 127) {
            tip += part;
        }
    }
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static std::vector<std::string> ParseCsvLine(const std::string& rawLine) {
    std::vector<std::string> cols;
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    size_t i = 0, n = line.size();
    while (i <= n) {
        if (i == n) {
            cols.push_back("");
            break;
        }
        if (line[i] == '\'') {
            size_t start = i + 1;
            size_t close = std::string::npos;
            for (size_t j = start; j < n; ++j) {
                if (line[j] == '\'' && (j + 1 == n || line[j + 1] == ',')) {
                    close = j;
                    break;
                }
            }
            if (close != std::string::npos) {
                cols.push_back(line.substr(start, close - start));
                i = close + 1;
                if (i < n && line[i] == ',') ++i;
                else break;
            } else {
                cols.push_back(line.substr(start));
                break;
            }
        } else {
            size_t comma = line.find(',', i);
            if (comma == std::string::npos) {
                cols.push_back(line.substr(i));
                break;
            } else {
                cols.push_back(line.substr(i, comma - i));
                i = comma + 1;
            }
        }
    }
    return cols;
}

static std::vector<AlienFX_SDK::Afx_light> BuildDarfonPerKeyTemplate() {
    struct DarfonKeyDef { byte id; WORD flags; WORD scancode; const char* name; };
    static const DarfonKeyDef kKeys[] = {
        { 0,   0, 27,  "Esc" },
        { 1,   0, 112, "F1" }, { 2,  0, 113, "F2" }, { 3,  0, 114, "F3" }, { 4,  0, 115, "F4" },
        { 5,   0, 116, "F5" }, { 6,  0, 117, "F6" }, { 7,  0, 118, "F7" }, { 8,  0, 119, "F8" },
        { 9,   0, 120, "F9" }, { 10, 0, 121, "F10" }, { 11, 0, 122, "F11" }, { 12, 0, 123, "F12" },
        { 13,  0, 292, "Home" }, { 14, 0, 291, "End" }, { 15, 0, 302, "Delete" },
        { 16,  0, 173, "Mute" }, { 17, 0, 430, "VolumeDn" }, { 18, 0, 431, "VolumeUp" }, { 19, 0, 0, "Microphone" },
        { 20,  0, 192, "`" },
        { 21,  0, 49, "1" }, { 22, 0, 50, "2" }, { 23, 0, 51, "3" }, { 24, 0, 52, "4" }, { 25, 0, 53, "5" },
        { 26,  0, 54, "6" }, { 27, 0, 55, "7" }, { 28, 0, 56, "8" }, { 29, 0, 57, "9" }, { 30, 0, 48, "0" },
        { 31,  0, 189, "-" }, { 32, 0, 187, "=" },
        { 34,  0, 8, "Backspace" }, { 35, 0, 8, "Backspace 2" },
        { 36,  0, 400, "Num Lock" }, { 37, 0, 367, "Num /" }, { 38, 0, 106, "Num *" }, { 39, 0, 109, "Num -" },
        { 40,  0, 9, "Tab" }, { 41, 0, 9, "Tab 2" },
        { 42,  0, 81, "Q" }, { 43, 0, 87, "W" }, { 44, 0, 69, "E" }, { 45, 0, 82, "R" }, { 46, 0, 84, "T" },
        { 47,  0, 89, "Y" }, { 48, 0, 85, "U" }, { 49, 0, 73, "I" }, { 50, 0, 79, "O" }, { 51, 0, 80, "P" },
        { 52,  0, 219, "[" }, { 53, 0, 221, "]" }, { 54, 0, 220, "\\ 2" }, { 55, 0, 220, "\\" },
        { 56,  0, 103, "Num 7" }, { 57, 0, 104, "Num 8" }, { 58, 0, 105, "Num 9" }, { 59, 0, 107, "Num + 2" },
        { 60,  0, 20, "Caps Lock" }, { 61, 0, 20, "Caps Lock 2" },
        { 62,  0, 65, "A" }, { 63, 0, 83, "S" }, { 64, 0, 68, "D" }, { 65, 0, 70, "F" }, { 66, 0, 71, "G" },
        { 67,  0, 72, "H" }, { 68, 0, 74, "J" }, { 69, 0, 75, "K" }, { 70, 0, 76, "L" },
        { 71,  0, 186, ";" }, { 72, 0, 222, "'" }, { 74, 0, 13, "Enter" },
        { 76,  0, 100, "Num 4" }, { 77, 0, 101, "Num 5" }, { 78, 0, 102, "Num 6" }, { 79, 0, 107, "Num +" },
        { 80,  0, 160, "Shift" }, { 81, 0, 160, "Shift 2" },
        { 83,  0, 90, "Z" }, { 84, 0, 88, "X" }, { 85, 0, 67, "C" }, { 86, 0, 86, "V" },
        { 87,  0, 66, "B" }, { 88, 0, 78, "N" }, { 89, 0, 77, "M" },
        { 90,  0, 188, "," }, { 91, 0, 190, "." }, { 92, 0, 191, "/" },
        { 93,  0, 417, "Right Shift 2" }, { 94, 0, 417, "Right Shift" },
        { 96,  0, 97, "Num 1" }, { 97, 0, 98, "Num 2" }, { 98, 0, 99, "Num 3" }, { 99, 0, 269, "Num Enter" },
        { 100, 0, 162, "Ctrl" }, { 101, 0, 255, "FN" },
        { 102, 0, 347, "Left Windows" }, { 103, 0, 347, "Left Windows 2" },
        { 104, 0, 164, "Alt" },
        { 105, 0, 32, "Space 3" }, { 106, 0, 32, "Space" }, { 107, 0, 32, "Space 2" }, { 108, 0, 32, "Space 4" },
        { 109, 0, 348, "Right Windows" }, { 111, 0, 421, "Right Alt" }, { 112, 0, 419, "Right Ctrl" },
        { 114, 0, 294, "Up" },
        { 116, 0, 96, "Num 0" }, { 117, 0, 96, "Num 0 2" }, { 118, 0, 110, "Num Del ." }, { 119, 0, 269, "Num Enter 2" },
        { 133, 0, 293, "Left" }, { 134, 0, 296, "Down" }, { 135, 0, 295, "Right" }
    };
    std::vector<AlienFX_SDK::Afx_light> out;
    out.reserve(sizeof(kKeys) / sizeof(kKeys[0]));
    for (const auto& k : kKeys) {
        AlienFX_SDK::Afx_light l{};
        l.lightid = k.id;
        l.flags = k.flags;
        l.scancode = k.scancode;
        l.name = k.name;
        out.push_back(l);
    }
    return out;
}

static void EnsureDefaultDeviceMappings() {
    if (!conf) return;
    std::wstring csvPath = GetExeDirectory() + L"\\Mappings\\devices.csv";
    std::string csvPathUtf8 = WideToUtf8(csvPath);
    std::vector<AlienFX_SDK::Afx_light> darfonTemplate = BuildDarfonPerKeyTemplate();

    bool anyAdded = false;
    for (auto& dev : conf->afx_dev.fxdevs) {
        if (!dev.present) continue;

        bool isDarfonKeyboard = (dev.vid == 0x0D62) || (dev.dev && dev.dev->version == AlienFX_SDK::API_V5);
        bool hasPlaceholder = std::any_of(dev.lights.begin(), dev.lights.end(),
            [](const AlienFX_SDK::Afx_light& l) {
                return l.name.rfind("LED Zone ", 0) == 0 || l.name.rfind("Light ", 0) == 0;
            });
        bool allZeroScancode = !dev.lights.empty() && std::all_of(dev.lights.begin(), dev.lights.end(),
            [](const AlienFX_SDK::Afx_light& l) { return l.scancode == 0; });
        bool needsUpgrade = dev.lights.empty() ||
            (isDarfonKeyboard && (hasPlaceholder || dev.lights.size() <= 20 || allZeroScancode));

        if (needsUpgrade) {
            dev.lights.clear();

            // 1. Try matching vid/pid in Mappings/devices.csv (first matching block)
            std::ifstream file(csvPathUtf8);
            if (file.is_open()) {
                std::string line;
                bool inMatchingDevice = false;
                bool foundDevice = false;
                while (std::getline(file, line)) {
                    if (line.empty()) continue;
                    std::vector<std::string> cols = ParseCsvLine(line);
                    if (cols.empty()) continue;

                    if (cols[0] == "0" && cols.size() >= 4) {
                        if (foundDevice) break;
                        int fvid = std::atoi(cols[1].c_str());
                        int fpid = std::atoi(cols[2].c_str());
                        inMatchingDevice = (fvid == dev.vid && fpid == dev.pid);
                        if (inMatchingDevice) foundDevice = true;
                    } else if (cols[0] == "3") {
                        if (inMatchingDevice) break;
                        inMatchingDevice = false;
                    } else if (inMatchingDevice && cols[0] == "1" && cols.size() >= 4) {
                        byte lid = static_cast<byte>(std::atoi(cols[1].c_str()));
                        bool dup = std::any_of(dev.lights.begin(), dev.lights.end(),
                            [lid](const AlienFX_SDK::Afx_light& x) { return x.lightid == lid; });
                        if (dup) continue;

                        AlienFX_SDK::Afx_light l{};
                        l.lightid = lid;
                        l.data = static_cast<DWORD>(std::strtoul(cols[2].c_str(), nullptr, 10));
                        l.name = cols[3];
                        if (isDarfonKeyboard) {
                            for (const auto& dk : darfonTemplate) {
                                if (dk.lightid == l.lightid) {
                                    if (l.scancode == 0) l.scancode = dk.scancode;
                                    if (l.name.empty() || l.name.rfind("Light ", 0) == 0) l.name = dk.name;
                                    break;
                                }
                            }
                        }
                        dev.lights.push_back(l);
                        anyAdded = true;
                    }
                }
            }

            // 2. Fallback: full per-key layout for Darfon/API_V5 keyboards, or 0..15 zones for other devices
            if (dev.lights.empty()) {
                if (isDarfonKeyboard) {
                    dev.lights = darfonTemplate;
                    anyAdded = true;
                } else {
                    for (byte i = 0; i < 16; ++i) {
                        AlienFX_SDK::Afx_light l{};
                        l.lightid = i;
                        l.flags = 0;
                        l.name = "LED Zone " + std::to_string(i);
                        dev.lights.push_back(l);
                    }
                    anyAdded = true;
                }
            }
        }

        // 3. Ensure full-size Darfon keyboards have Numpad & any missing template keys
        if (isDarfonKeyboard) {
            for (const auto& dk : darfonTemplate) {
                bool exists = std::any_of(dev.lights.begin(), dev.lights.end(),
                    [&](const AlienFX_SDK::Afx_light& x) { return x.lightid == dk.lightid; });
                if (!exists) {
                    dev.lights.push_back(dk);
                    anyAdded = true;
                }
            }
        }
    }
    if (anyAdded) {
        conf->afx_dev.SaveMappings();
    }
}

static void InitTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    UINT iconRes = (conf && !conf->lightsOn) ? IDI_ALIENFX_OFF : IDI_ALIENFX_ON;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(iconRes));
    wcscpy_s(g_nid.szTip, L"AlienCockpit — AWCC Alternative");
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded = true;
        UpdateTrayIconAndTooltip();
    }
}

static void RemoveTrayIcon() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

static void ShowMainWindow(HWND hwnd) {
    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Open AlienCockpit");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    if (conf) {
        for (size_t i = 0; i < conf->profiles.size() && i < 20; ++i) {
            UINT flags = MF_STRING;
            if (conf->activeProfile && conf->activeProfile->id == conf->profiles[i]->id)
                flags |= MF_CHECKED;
            std::wstring pname = L"Profile: " + Utf8ToWide(conf->profiles[i]->name);
            AppendMenuW(hMenu, flags, IDM_TRAY_PROF_BASE + i, pname.c_str());
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING | (conf->lightsOn ? MF_CHECKED : 0), IDM_TRAY_LIGHTS, L"Lights Enabled");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit AlienCockpit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

static void PostJsonToUI(const std::string& json) {
    if (g_webview) {
        std::wstring wjson = Utf8ToWide(json);
        g_webview->PostWebMessageAsJson(wjson.c_str());
    }
}

static void InitWebView(HWND hwnd) {
    std::wstring userDataFolder = GetWebViewUserDataFolder();
    HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxW(hwnd, L"Failed to initialize Microsoft Edge WebView2 Runtime.\nPlease ensure WebView2 Runtime is installed.", L"AlienCockpit", MB_OK | MB_ICONERROR);
                    return result;
                }
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) {
                                MessageBoxW(hwnd, L"Failed to create WebView2 controller.", L"AlienCockpit", MB_OK | MB_ICONERROR);
                                return res;
                            }
                            g_webviewController = controller;
                            g_webviewController->get_CoreWebView2(&g_webview);

                            RECT bounds{};
                            GetClientRect(hwnd, &bounds);
                            g_webviewController->put_Bounds(bounds);

                            // Set dark background to prevent white flash
                            ComPtr<ICoreWebView2Controller2> ctrl2;
                            if (SUCCEEDED(g_webviewController.As(&ctrl2))) {
                                COREWEBVIEW2_COLOR bg = { 255, 5, 7, 13 };
                                ctrl2->put_DefaultBackgroundColor(bg);
                            }

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings))) {
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            // Map virtual host to local ui/ directory
                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(g_webview.As(&wv3))) {
                                std::wstring uiDir = GetExeDirectory() + L"\\ui";
                                wv3->SetVirtualHostNameToFolderMapping(
                                    L"aliencockpit.local", uiDir.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [hwnd](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msgRaw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&msgRaw)) && msgRaw) {
                                            std::string jsonStr = WideToUtf8(msgRaw);
                                            CoTaskMemFree(msgRaw);
                                            if (jsonStr.find("\"windowDrag\"") != std::string::npos) {
                                                ReleaseCapture();
                                                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                                return S_OK;
                                            }
                                            if (jsonStr.find("\"windowMinimize\"") != std::string::npos) {
                                                ShowWindow(hwnd, SW_MINIMIZE);
                                                return S_OK;
                                            }
                                            if (jsonStr.find("\"windowMaximize\"") != std::string::npos) {
                                                ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                                                return S_OK;
                                            }
                                            if (jsonStr.find("\"windowClose\"") != std::string::npos) {
                                                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                                                return S_OK;
                                            }
                                            if (jsonStr.find("\"restartAsAdmin\"") != std::string::npos) {
                                                RelaunchAsAdmin(hwnd, false);
                                                return S_OK;
                                            }
                                            std::string resp = CockpitBridge::HandleMessage(jsonStr);
                                            if (jsonStr.find("\"toggleLights\"") != std::string::npos ||
                                                jsonStr.find("\"activateProfile\"") != std::string::npos ||
                                                jsonStr.find("\"applyGamePreset\"") != std::string::npos ||
                                                jsonStr.find("\"restorePreset\"") != std::string::npos ||
                                                jsonStr.find("\"setBrightness\"") != std::string::npos) {
                                                UpdateTrayIconAndTooltip(false);
                                            }
                                            PostJsonToUI(resp);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            g_webview->Navigate(L"https://aliencockpit.local/index.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hrEnv)) {
        MessageBoxW(hwnd, L"Could not start WebView2 Environment. Please install the Microsoft Edge WebView2 Runtime.", L"AlienCockpit", MB_OK | MB_ICONERROR);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            NCCALCSIZE_PARAMS* pncsp = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            UINT dpi = GetDpiForWindow(hwnd);
            int frameX = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int frameY = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (IsZoomed(hwnd)) {
                pncsp->rgrc[0].left += frameX;
                pncsp->rgrc[0].top += frameY;
                pncsp->rgrc[0].right -= frameX;
                pncsp->rgrc[0].bottom -= frameY;
            } else {
                pncsp->rgrc[0].left += frameX;
                pncsp->rgrc[0].right -= frameX;
                pncsp->rgrc[0].bottom -= frameY;
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        if (IsZoomed(hwnd)) return HTCLIENT;
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        UINT dpi = GetDpiForWindow(hwnd);
        int border = MulDiv(6, dpi, 96);
        bool top = pt.y < border;
        bool bottom = pt.y >= rc.bottom - border;
        bool left = pt.x < border;
        bool right = pt.x >= rc.right - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetDpiForWindow(hwnd);
        if (!dpi) dpi = 96;
        mmi->ptMinTrackSize.x = MulDiv(960, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(640, dpi, 96);
        return 0;
    }

    case WM_SIZE:
        if (g_webviewController) {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_webviewController->put_Bounds(bounds);
        }
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (g_webviewController) {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_webviewController->put_Bounds(bounds);
        }
        return 0;
    }

    case WM_CLOSE:
        // Minimize to tray instead of quitting
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TELEMETRY) {
            UpdateTrayIconAndTooltip();
            if (IsWindowVisible(hwnd) && g_webview) {
                PostJsonToUI(CockpitBridge::BuildTelemetryJson());
            }
        } else if (wParam == TIMER_EFFECTS) {
            CockpitBridge::StepAnimatedEffects();
        }
        return 0;

    case WM_DEVICECHANGE:
        if (conf && fxhl && wParam == 0x0007 /* DBT_DEVNODES_CHANGED */) {
            if (conf->afx_dev.AlienFXEnumDevices(mon ? mon->acpi : nullptr)) {
                EnsureDefaultDeviceMappings();
                fxhl->ClearAndRefresh(true);
                if (g_webview) {
                    PostJsonToUI(CockpitBridge::BuildFullStateJson());
                }
            }
            UpdateTrayIconAndTooltip(true);
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ShowMainWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wParam);
        if (id == IDM_TRAY_OPEN) {
            ShowMainWindow(hwnd);
        } else if (id == IDM_TRAY_LIGHTS && conf && fxhl) {
            conf->lightsOn = conf->lightsOn ? 0 : 1;
            fxhl->SetState(true);
            if (conf->lightsOn) {
                fxhl->ClearAndRefresh(true);
            }
            conf->Save();
            UpdateTrayIconAndTooltip(false);
            PostJsonToUI(CockpitBridge::BuildFullStateJson());
        } else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
        } else if (id >= IDM_TRAY_PROF_BASE && id < IDM_TRAY_PROF_BASE + 20 && conf && eve) {
            size_t idx = id - IDM_TRAY_PROF_BASE;
            if (idx < conf->profiles.size()) {
                eve->SwitchActiveProfile(conf->profiles[idx], true);
                UpdateTrayIconAndTooltip(false);
                PostJsonToUI(CockpitBridge::BuildFullStateJson());
            }
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TELEMETRY);
        KillTimer(hwnd, TIMER_EFFECTS);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    bool isRelaunch = (lpCmdLine && wcsstr(lpCmdLine, L"--relaunch") != nullptr);
    bool noElevate = (lpCmdLine && wcsstr(lpCmdLine, L"--no-elevate") != nullptr);
    bool startMin = (lpCmdLine && wcsstr(lpCmdLine, L"--minimized") != nullptr);

    g_hMutex = CreateMutexW(nullptr, TRUE, L"Local\\AlienCockpitSingleInstance");
    DWORD mutexErr = GetLastError();
    if (mutexErr == ERROR_ALREADY_EXISTS && isRelaunch) {
        for (int retry = 0; retry < 30; ++retry) {
            if (g_hMutex) {
                CloseHandle(g_hMutex);
                g_hMutex = nullptr;
            }
            Sleep(100);
            g_hMutex = CreateMutexW(nullptr, TRUE, L"Local\\AlienCockpitSingleInstance");
            mutexErr = GetLastError();
            if (mutexErr != ERROR_ALREADY_EXISTS) break;
        }
    }
    if (mutexErr == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"AlienCockpitWindowClass", nullptr);
        if (existing) {
            ShowMainWindow(existing);
        }
        if (g_hMutex) {
            CloseHandle(g_hMutex);
            g_hMutex = nullptr;
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Initialize AlienFX & AlienFan backend
    conf = new ConfigHandler();
    conf->Load();

    if (conf->alwaysAdmin && !IsUserAnAdmin() && !noElevate) {
        if (RelaunchAsAdmin(nullptr, startMin)) {
            delete conf;
            conf = nullptr;
            CoUninitialize();
            return 0;
        }
    }

    mon = new MonHelper();
    fxhl = new FXHelper();
    eve = new EventHandler();
    if (conf->fanControl && mon) {
        eve->ToggleFans();
    }
    conf->afx_dev.AlienFXEnumDevices(mon ? mon->acpi : nullptr);
    EnsureDefaultDeviceMappings();
    fxhl->Start();
    eve->StartProfiles();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ALIENCOCKPIT));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(5, 7, 13));
    wc.lpszClassName = L"AlienCockpitWindowClass";
    RegisterClassExW(&wc);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    UINT sysDpi = GetDpiForSystem();
    if (!sysDpi) sysDpi = 96;
    int waW = wa.right - wa.left;
    int waH = wa.bottom - wa.top;
    int winW = MulDiv(1400, sysDpi, 96);
    int winH = MulDiv(880, sysDpi, 96);
    if (winW > waW - 24) winW = waW - 24;
    if (winH > waH - 24) winH = waH - 24;
    int winX = wa.left + (waW - winW) / 2;
    int winY = wa.top + (waH - winH) / 2;

    HWND hwnd = CreateWindowExW(
        0,
        L"AlienCockpitWindowClass",
        L"AlienCockpit",
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, hInstance, nullptr);

    mDlg = hwnd;

    // Enable Windows 11 Immersive Dark Mode, Rounded Corners & Seamless Border
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    DWORD cornerPref = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
    MARGINS margins = { 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

    InitTrayIcon(hwnd);
    InitWebView(hwnd);
    SetTimer(hwnd, TIMER_TELEMETRY, 1500, nullptr);
    SetTimer(hwnd, TIMER_EFFECTS, 33, nullptr);

    if (!startMin) {
        ShowWindow(hwnd, nCmdShow ? nCmdShow : SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        SetForegroundWindow(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Graceful shutdown
    if (eve) {
        eve->StopProfiles();
        delete eve;
        eve = nullptr;
    }
    if (fxhl) {
        fxhl->Stop();
        delete fxhl;
        fxhl = nullptr;
    }
    if (mon) {
        delete mon;
        mon = nullptr;
    }
    if (conf) {
        conf->Save();
        delete conf;
        conf = nullptr;
    }

    CoUninitialize();
    if (g_hMutex) {
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }
    return static_cast<int>(msg.wParam);
}
