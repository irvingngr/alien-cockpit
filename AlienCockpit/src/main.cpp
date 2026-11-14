#include <windows.h>
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
#include "MonHelper.h"
#include "ConfigFan.h"

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

static void InitTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ALIENCOCKPIT));
    wcscpy_s(g_nid.szTip, L"AlienCockpit — Space Command Center");
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded = true;
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
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR msgRaw = nullptr;
                                        if (SUCCEEDED(args->get_WebMessageAsJson(&msgRaw)) && msgRaw) {
                                            std::string jsonStr = WideToUtf8(msgRaw);
                                            CoTaskMemFree(msgRaw);
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
        if (wParam == TIMER_TELEMETRY && IsWindowVisible(hwnd) && g_webview) {
            PostJsonToUI(CockpitBridge::BuildTelemetryJson());
        }
        return 0;

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
            fxhl->SetState();
            conf->Save();
            PostJsonToUI(CockpitBridge::BuildFullStateJson());
        } else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
        } else if (id >= IDM_TRAY_PROF_BASE && id < IDM_TRAY_PROF_BASE + 20 && conf && eve) {
            size_t idx = id - IDM_TRAY_PROF_BASE;
            if (idx < conf->profiles.size()) {
                eve->SwitchActiveProfile(conf->profiles[idx], true);
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
    fxhl = new FXHelper();
    eve = new EventHandler();
    if (conf->fanControl) {
        eve->ToggleFans();
    }
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

    HWND hwnd = CreateWindowExW(
        0,
        L"AlienCockpitWindowClass",
        L"AlienCockpit",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, hInstance, nullptr);

    mDlg = hwnd;

    // Enable Windows 11 Immersive Dark Titlebar
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

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
    if (conf) {
        conf->Save();
        delete conf;
        conf = nullptr;
    }

    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
