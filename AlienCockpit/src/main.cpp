#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
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
static const UINT_PTR TIMER_TELEMETRY = 1;

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

static void UpdateTrayIconAndTooltip() {
    if (!g_trayAdded || !conf) return;
    UINT iconRes = conf->lightsOn ? (conf->stateDimmed ? IDI_ALIENFX_DIM : IDI_ALIENFX_ON) : IDI_ALIENFX_OFF;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(iconRes));

    std::wstring tip = L"AlienCockpit | Lights: ";
    tip += (conf->lightsOn ? L"ON" : L"OFF");

    auto batts = BatteryMonitor::GetAllBatteries();
    for (const auto& b : batts) {
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

static void EnsureDefaultDeviceMappings() {
    if (!conf) return;
    std::wstring csvPath = GetExeDirectory() + L"\\Mappings\\devices.csv";
    std::string csvPathUtf8 = WideToUtf8(csvPath);

    bool anyAdded = false;
    for (auto& dev : conf->afx_dev.fxdevs) {
        if (!dev.present || !dev.lights.empty()) continue;

        // 1. Try matching vid/pid in Mappings/devices.csv
        std::ifstream file(csvPathUtf8);
        if (file.is_open()) {
            std::string line;
            bool inMatchingDevice = false;
            while (std::getline(file, line)) {
                if (line.empty()) continue;
                std::vector<std::string> cols;
                std::string cur;
                bool inQuotes = false;
                for (char ch : line) {
                    if (ch == '\'') { inQuotes = !inQuotes; }
                    else if (ch == ',' && !inQuotes) { cols.push_back(cur); cur.clear(); }
                    else if (ch != '\r' && ch != '\n') { cur.push_back(ch); }
                }
                cols.push_back(cur);
                if (cols.empty()) continue;

                if (cols[0] == "0" && cols.size() >= 4) {
                    int fvid = std::atoi(cols[1].c_str());
                    int fpid = std::atoi(cols[2].c_str());
                    inMatchingDevice = (fvid == dev.vid && fpid == dev.pid);
                } else if (cols[0] == "3") {
                    inMatchingDevice = false;
                } else if (inMatchingDevice && cols[0] == "1" && cols.size() >= 4) {
                    AlienFX_SDK::Afx_light l{};
                    l.lightid = static_cast<byte>(std::atoi(cols[1].c_str()));
                    l.flags = static_cast<WORD>(std::atoi(cols[2].c_str()));
                    l.name = cols[3];
                    dev.lights.push_back(l);
                    anyAdded = true;
                }
            }
        }

        // 2. Fallback: populate default probeable zones (0..15) for new/unmapped devices (e.g. 2025 AW-ELC)
        if (dev.lights.empty()) {
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
    std::wstring userDataFolder = GetExeDirectory() + L"\\WebView2Data";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) return res;
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
                                            std::string resp = CockpitBridge::HandleMessage(jsonStr);
                                            PostJsonToUI(resp);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            g_webview->Navigate(L"https://aliencockpit.local/index.html");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
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
            UpdateTrayIconAndTooltip();
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
            UpdateTrayIconAndTooltip();
            PostJsonToUI(CockpitBridge::BuildFullStateJson());
        } else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
        } else if (id >= IDM_TRAY_PROF_BASE && id < IDM_TRAY_PROF_BASE + 20 && conf && eve) {
            size_t idx = id - IDM_TRAY_PROF_BASE;
            if (idx < conf->profiles.size()) {
                eve->SwitchActiveProfile(conf->profiles[idx], true);
                UpdateTrayIconAndTooltip();
                PostJsonToUI(CockpitBridge::BuildFullStateJson());
            }
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TELEMETRY);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\AlienCockpitSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"AlienCockpitWindowClass", nullptr);
        if (existing) {
            ShowMainWindow(existing);
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Initialize AlienFX & AlienFan backend
    conf = new ConfigHandler();
    conf->Load();
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

    bool startMin = (lpCmdLine && wcsstr(lpCmdLine, L"--minimized") != nullptr) || (conf->startMinimized != 0);
    if (!startMin) {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
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
    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
