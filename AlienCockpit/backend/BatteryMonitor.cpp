#include "BatteryMonitor.h"
#include <windows.h>
#include <initguid.h>
#include <winioctl.h>
#include <batclass.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "hid.lib")

#ifndef DEVPKEY_Device_BatteryPercentage
// {104EA319-6EE2-4701-BD47-8DDBF425BBE5}, 2
static const DEVPROPKEY PKEY_BatteryPercentage = {
    { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2
};
#else
static const DEVPROPKEY PKEY_BatteryPercentage = DEVPKEY_Device_BatteryPercentage;
#endif

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static std::string ClassifyDeviceType(const std::string& name) {
    std::string lower = ToLowerAscii(name);
    if (lower.find("mouse") != std::string::npos || lower.find("raton") != std::string::npos ||
        lower.find("aw6") != std::string::npos || lower.find("aw7") != std::string::npos ||
        lower.find("aw3") != std::string::npos || lower.find("pro wireless") != std::string::npos ||
        lower.find("superlight") != std::string::npos)
        return "mouse";
    if (lower.find("keyboard") != std::string::npos || lower.find("teclado") != std::string::npos ||
        lower.find("aw5") != std::string::npos || lower.find("aw9") != std::string::npos ||
        lower.find("aw4") != std::string::npos)
        return "keyboard";
    if (lower.find("xbox") != std::string::npos || lower.find("dualsense") != std::string::npos ||
        lower.find("dualshock") != std::string::npos || lower.find("controller") != std::string::npos ||
        lower.find("gamepad") != std::string::npos || lower.find("mando") != std::string::npos ||
        lower.find("joystick") != std::string::npos || lower.find("joy-con") != std::string::npos ||
        lower.find("8bitdo") != std::string::npos)
        return "controller";
    if (lower.find("headset") != std::string::npos || lower.find("audio") != std::string::npos ||
        lower.find("headphone") != std::string::npos || lower.find("aw920") != std::string::npos ||
        lower.find("aw720h") != std::string::npos || lower.find("astro") != std::string::npos ||
        lower.find("a50") != std::string::npos || lower.find("a30") != std::string::npos ||
        lower.find("a20") != std::string::npos || lower.find("arctis") != std::string::npos ||
        lower.find("airpods") != std::string::npos || lower.find("earbud") != std::string::npos ||
        lower.find("buds") != std::string::npos || lower.find("audifono") != std::string::npos)
        return "headset";
    return "peripheral";
}

static void AddUniqueBattery(std::vector<BatteryDeviceInfo>& result, const BatteryDeviceInfo& info) {
    for (auto& existing : result) {
        if (ToLowerAscii(existing.name) == ToLowerAscii(info.name)) {
            if (existing.percentage < 0 && info.percentage >= 0) {
                existing = info;
            }
            return;
        }
    }
    result.push_back(info);
}

static std::string ResolveParentUsbName(DEVINST devInst) {
    DEVINST cur = devInst;
    for (int depth = 0; depth < 3; ++depth) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS || !parent) break;
        wchar_t buf[256]{};
        ULONG sz = sizeof(buf);
        if (CM_Get_DevNode_Registry_PropertyW(parent, CM_DRP_FRIENDLYNAME, nullptr, buf, &sz, 0) == CR_SUCCESS && buf[0] != L'\0') {
            return WideToUtf8(buf);
        }
        sz = sizeof(buf);
        if (CM_Get_DevNode_Registry_PropertyW(parent, CM_DRP_DEVICEDESC, nullptr, buf, &sz, 0) == CR_SUCCESS && buf[0] != L'\0') {
            std::string desc = WideToUtf8(buf);
            if (desc.find("USB Input Device") == std::string::npos && desc.find("HID-compliant") == std::string::npos) {
                return desc;
            }
        }
        cur = parent;
    }
    return {};
}

static int NormalizeHidPercent(ULONG raw, const HIDP_VALUE_CAPS& vc) {
    if (vc.LogicalMax > vc.LogicalMin) {
        double ratio = static_cast<double>(static_cast<LONG>(raw) - vc.LogicalMin) /
                       static_cast<double>(vc.LogicalMax - vc.LogicalMin);
        int pct = static_cast<int>(std::lround(ratio * 100.0));
        return std::clamp(pct, 0, 100);
    }
    return (raw <= 100) ? static_cast<int>(raw) : -1;
}

static bool IsBatteryUsage(const HIDP_VALUE_CAPS& vc) {
    if (vc.IsRange) return false;
    if (vc.UsagePage == 0x06 && vc.NotRange.Usage == 0x20) return true;
    if (vc.UsagePage == 0x85 && (vc.NotRange.Usage == 0x64 || vc.NotRange.Usage == 0x65 ||
        vc.NotRange.Usage == 0x66 || vc.NotRange.Usage == 0x68)) return true;
    return false;
}

static bool TryReadHidBatteryReport(HANDLE hDev, PHIDP_PREPARSED_DATA preparsed, const HIDP_CAPS& caps,
    HIDP_REPORT_TYPE rType, const std::string& fallbackName, DWORD idx, std::vector<BatteryDeviceInfo>& result) {

    USHORT numCaps = (rType == HidP_Input) ? caps.NumberInputValueCaps : caps.NumberFeatureValueCaps;
    USHORT reportLen = (rType == HidP_Input) ? caps.InputReportByteLength : caps.FeatureReportByteLength;
    if (numCaps == 0 || reportLen == 0) return false;

    std::vector<HIDP_VALUE_CAPS> vCaps(numCaps);
    if (HidP_GetValueCaps(rType, vCaps.data(), &numCaps, preparsed) != HIDP_STATUS_SUCCESS)
        return false;

    for (USHORT c = 0; c < numCaps; ++c) {
        if (!IsBatteryUsage(vCaps[c])) continue;

        std::vector<BYTE> report(reportLen, 0);
        report[0] = vCaps[c].ReportID;
        BOOL readOk = (rType == HidP_Input)
            ? HidD_GetInputReport(hDev, report.data(), static_cast<ULONG>(report.size()))
            : HidD_GetFeature(hDev, report.data(), static_cast<ULONG>(report.size()));
        if (!readOk) continue;

        ULONG rawVal = 0;
        if (HidP_GetUsageValue(rType, vCaps[c].UsagePage, vCaps[c].LinkCollection,
            vCaps[c].NotRange.Usage, &rawVal, preparsed,
            reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS) {
            continue;
        }

        int pct = NormalizeHidPercent(rawVal, vCaps[c]);
        if (pct < 0 || pct > 100) continue;

        wchar_t prod[128]{};
        std::string pname = fallbackName.empty() ? "HID Wireless Device" : fallbackName;
        if (HidD_GetProductString(hDev, prod, sizeof(prod)) && prod[0] != L'\0') {
            pname = WideToUtf8(prod);
        }

        BatteryDeviceInfo info;
        info.id = std::string(rType == HidP_Input ? "hid-in-" : "hid-feat-") + std::to_string(idx);
        info.name = pname;
        info.type = ClassifyDeviceType(pname);
        info.percentage = pct;
        info.charging = false;
        info.acOnline = false;
        AddUniqueBattery(result, info);
        return true;
    }
    return false;
}

static bool OverlappedReadOnce(HANDLE hDev, std::vector<BYTE>& inBuf, DWORD timeoutMs) {
    if (hDev == INVALID_HANDLE_VALUE || inBuf.empty()) return false;
    HANDLE hEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvt) return false;

    OVERLAPPED ov{};
    ov.hEvent = hEvt;
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hDev, inBuf.data(), static_cast<DWORD>(inBuf.size()), &readBytes, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(hEvt, timeoutMs) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(hDev, &ov, &readBytes, FALSE);
        } else {
            CancelIoEx(hDev, &ov);
            ok = FALSE;
        }
    }
    CloseHandle(hEvt);
    return (ok != FALSE) && (readBytes > 0);
}

static bool OverlappedWriteAndRead(HANDLE hDev, const std::vector<BYTE>& outBuf, std::vector<BYTE>& inBuf, DWORD timeoutMs) {
    if (hDev == INVALID_HANDLE_VALUE || outBuf.empty() || inBuf.empty()) return false;
    HANDLE hEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvt) return false;

    OVERLAPPED ovW{};
    ovW.hEvent = hEvt;
    DWORD written = 0;
    BOOL wOk = WriteFile(hDev, outBuf.data(), static_cast<DWORD>(outBuf.size()), &written, &ovW);
    if (!wOk && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(hEvt, timeoutMs) == WAIT_OBJECT_0) {
            wOk = GetOverlappedResult(hDev, &ovW, &written, FALSE);
        } else {
            CancelIoEx(hDev, &ovW);
            wOk = FALSE;
        }
    }
    if (!wOk) {
        CloseHandle(hEvt);
        return false;
    }

    ResetEvent(hEvt);
    OVERLAPPED ovR{};
    ovR.hEvent = hEvt;
    DWORD readBytes = 0;
    BOOL rOk = ReadFile(hDev, inBuf.data(), static_cast<DWORD>(inBuf.size()), &readBytes, &ovR);
    if (!rOk && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(hEvt, timeoutMs) == WAIT_OBJECT_0) {
            rOk = GetOverlappedResult(hDev, &ovR, &readBytes, FALSE);
        } else {
            CancelIoEx(hDev, &ovR);
            rOk = FALSE;
        }
    }
    CloseHandle(hEvt);
    return (rOk != FALSE) && (readBytes > 0);
}

static int ExtractBatteryPercentAndCharging(const std::vector<BYTE>& buf, bool isPrimax, bool& chargingOut) {
    chargingOut = false;
    if (buf.size() < 6) return -1;
    bool allZero = std::all_of(buf.begin() + 1, buf.end(), [](BYTE b) { return b == 0; });
    if (allZero) return -1;

    if (isPrimax) {
        // Primax / AlienFX V7 (0x40 0x03 status response)
        if (buf[1] == 0x40 || buf[0] == 0x40) {
            const size_t base = (buf[0] == 0x40) ? 0 : 1;
            if (base + 6 < buf.size()) {
                BYTE st = buf[base + 3];
                if (st == 0x01 || st == 0x08) chargingOut = true;
            }
            const size_t idxList[] = { base + 4, base + 5, base + 6, base + 7 };
            for (size_t idx : idxList) {
                if (idx < buf.size() && buf[idx] >= 1 && buf[idx] <= 100) {
                    return static_cast<int>(buf[idx]);
                }
            }
        }
        return -1;
    }

    // Darfon 0x0D62:0x9ABC telemetry (echoes 0x20 or command in [1]/[2])
    const size_t candIdx[] = { 4, 5, 6, 3 };
    if (buf[1] == 0x20 || buf[2] == 0x20 || buf[2] != 0 || buf[3] != 0) {
        if (buf[3] == 0x01 || buf[7] == 0x01) chargingOut = true;
        for (size_t idx : candIdx) {
            if (idx < buf.size() && buf[idx] >= 1 && buf[idx] <= 100) {
                return static_cast<int>(buf[idx]);
            }
        }
        if (buf[1] == 0x20) {
            for (size_t idx : candIdx) {
                if (idx < buf.size() && buf[idx] <= 100) {
                    return static_cast<int>(buf[idx]);
                }
            }
        }
    }
    return -1;
}

static bool QueryAW610MBattery(std::vector<BatteryDeviceInfo>& result) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hHidInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hHidInfo == INVALID_HANDLE_VALUE) return false;

    struct CandidatePath {
        std::wstring path;
        WORD vid = 0;
        WORD pid = 0;
        int priority = 5;
    };
    std::vector<CandidatePath> candidates;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hHidInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
        DWORD reqLen = 0;
        SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, nullptr, 0, &reqLen, nullptr);
        if (reqLen == 0) continue;

        std::vector<BYTE> detailBuf(reqLen);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, detail, reqLen, nullptr, nullptr))
            continue;

        std::string pathLower = ToLowerAscii(WideToUtf8(detail->DevicePath));
        bool isDarfon610 = (pathLower.find("vid_0d62") != std::string::npos &&
            (pathLower.find("pid_9abc") != std::string::npos || pathLower.find("pid_a610") != std::string::npos));
        bool isPrimax610 = (pathLower.find("vid_0461") != std::string::npos &&
            (pathLower.find("pid_4ec0") != std::string::npos || pathLower.find("pid_4ec1") != std::string::npos));
        if (!isDarfon610 && !isPrimax610)
            continue;

        int prio = isPrimax610 ? 0 : 3;
        if (isDarfon610) {
            if (pathLower.find("col02") != std::string::npos) prio = 1;
            else if (pathLower.find("col01") != std::string::npos) prio = 2;
        }
        candidates.push_back({ detail->DevicePath, static_cast<WORD>(isPrimax610 ? 0x0461 : 0x0D62), 0, prio });
    }
    SetupDiDestroyDeviceInfoList(hHidInfo);

    if (candidates.empty()) return false;

    std::sort(candidates.begin(), candidates.end(),
        [](const CandidatePath& a, const CandidatePath& b) { return a.priority < b.priority; });

    bool sawSleep = false;
    std::string deviceLabel = "Alienware AW610M Wireless Mouse";

    for (const auto& cand : candidates) {
        HANDLE hDev = CreateFileW(cand.path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        bool canRW = (hDev != INVALID_HANDLE_VALUE);
        if (!canRW) {
            hDev = CreateFileW(cand.path.c_str(), 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (hDev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(HIDD_ATTRIBUTES);
        if (!HidD_GetAttributes(hDev, &attrs)) {
            CloseHandle(hDev);
            continue;
        }
        bool isDarfon = (attrs.VendorID == 0x0D62 && (attrs.ProductID == 0x9ABC || attrs.ProductID == 0xA610));
        bool isPrimax = (attrs.VendorID == 0x0461 && (attrs.ProductID == 0x4EC0 || attrs.ProductID == 0x4EC1));
        if (!isDarfon && !isPrimax) {
            CloseHandle(hDev);
            continue;
        }

        wchar_t prodStr[128]{};
        if (HidD_GetProductString(hDev, prodStr, sizeof(prodStr)) && prodStr[0] != L'\0') {
            std::string pName = WideToUtf8(prodStr);
            if (!pName.empty()) deviceLabel = pName;
        }

        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(hDev, &preparsed) && preparsed) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }

        // Skip OS pointer-only collection unless no vendor collection exists
        if (caps.UsagePage == 0x01 && caps.Usage == 0x02) {
            CloseHandle(hDev);
            continue;
        }

        bool gotLive = false;
        int pct = -1;
        bool charging = false;

        // 1. Overlapped Output/Input Interrupt Pipe (used by Primax 0x0461:0x4EC0 & Dongle vendor pipe)
        if (canRW && caps.OutputReportByteLength > 0 && caps.InputReportByteLength > 0) {
            DWORD outLen = caps.OutputReportByteLength;
            DWORD inLen = caps.InputReportByteLength;
            struct CmdProbe { BYTE b0, b1, b2, b3, b4; };
            const CmdProbe cmds[] = {
                { 0x00, 0x40, 0x03, 0x01, 0x00 },
                { 0x40, 0x03, 0x01, 0x00, 0x01 },
                { 0x00, 0x40, 0x02, 0x00, 0x00 },
                { 0x22, 0x20, 0x00, 0x00, 0x00 },
                { 0x02, 0x20, 0x00, 0x00, 0x00 }
            };
            for (const auto& cmd : cmds) {
                if ((cmd.b0 == 0x22 || cmd.b0 == 0x02) && !isDarfon) continue;
                if ((cmd.b0 == 0x40 || cmd.b1 == 0x40) && !isPrimax && outLen < 16) continue;
                std::vector<BYTE> outBuf(outLen, 0);
                outBuf[0] = cmd.b0;
                if (outLen > 1) outBuf[1] = cmd.b1;
                if (outLen > 2) outBuf[2] = cmd.b2;
                if (outLen > 3) outBuf[3] = cmd.b3;
                if (outLen > 4) outBuf[4] = cmd.b4;
                std::vector<BYTE> inBuf(inLen, 0);
                if (OverlappedWriteAndRead(hDev, outBuf, inBuf, 45)) {
                    pct = ExtractBatteryPercentAndCharging(inBuf, isPrimax || cmd.b0 == 0x40 || cmd.b1 == 0x40, charging);
                    if (pct >= 0 && pct <= 100) {
                        gotLive = true;
                        break;
                    }
                    if (std::all_of(inBuf.begin() + 1, inBuf.end(), [](BYTE b) { return b == 0; })) {
                        sawSleep = true;
                    }
                }
            }
        }

        // 2. Output + Input Control Transfer (HidD_SetOutputReport / HidD_GetInputReport)
        if (!gotLive && caps.OutputReportByteLength >= 8 && caps.InputReportByteLength >= 8) {
            const BYTE rids[] = { 0x22, 0x02, 0x00 };
            for (BYTE rid : rids) {
                std::vector<BYTE> outBuf(caps.OutputReportByteLength, 0);
                outBuf[0] = rid;
                if (rid == 0x00 && isPrimax) {
                    outBuf[1] = 0x40; outBuf[2] = 0x03; outBuf[3] = 0x01;
                } else {
                    outBuf[1] = 0x20;
                }
                if (HidD_SetOutputReport(hDev, outBuf.data(), static_cast<ULONG>(outBuf.size()))) {
                    std::vector<BYTE> inBuf(caps.InputReportByteLength, 0);
                    inBuf[0] = rid;
                    if (HidD_GetInputReport(hDev, inBuf.data(), static_cast<ULONG>(inBuf.size()))) {
                        pct = ExtractBatteryPercentAndCharging(inBuf, isPrimax, charging);
                        if (pct >= 0 && pct <= 100) {
                            gotLive = true;
                            break;
                        }
                    }
                }
            }
        }

        // 3. Feature Report Transfer (HidD_SetFeature / HidD_GetFeature)
        if (!gotLive) {
            std::vector<ULONG> lengths;
            if (caps.FeatureReportByteLength > 0) lengths.push_back(caps.FeatureReportByteLength);
            if (caps.FeatureReportByteLength != 65) lengths.push_back(65);
            if (caps.FeatureReportByteLength != 33) lengths.push_back(33);

            const BYTE reportIds[] = { 0x22, 0x02, 0x00 };
            for (BYTE rid : reportIds) {
                for (ULONG repLen : lengths) {
                    if (repLen < 7) continue;
                    std::vector<BYTE> outBuf(repLen, 0);
                    outBuf[0] = rid;
                    outBuf[1] = (rid == 0x00 && isPrimax) ? 0x40 : 0x20;
                    if (!HidD_SetFeature(hDev, outBuf.data(), repLen)) continue;

                    std::vector<BYTE> inBuf(repLen, 0);
                    inBuf[0] = rid;
                    if (!HidD_GetFeature(hDev, inBuf.data(), repLen)) continue;

                    if (std::all_of(inBuf.begin() + 1, inBuf.end(), [](BYTE b) { return b == 0; })) {
                        sawSleep = true;
                        continue;
                    }
                    pct = ExtractBatteryPercentAndCharging(inBuf, isPrimax, charging);
                    if (pct >= 0 && pct <= 100) {
                        gotLive = true;
                        break;
                    }
                }
                if (gotLive) break;
            }
        }

        CloseHandle(hDev);
        if (gotLive && pct >= 0) {
            BatteryDeviceInfo info;
            info.id = isPrimax ? "aw610m-primax" : "aw610m-darfon";
            info.name = deviceLabel;
            info.type = "mouse";
            info.percentage = pct;
            info.charging = charging || isPrimax;
            info.acOnline = isPrimax;
            AddUniqueBattery(result, info);
            return true;
        }
    }

    if (sawSleep) {
        BatteryDeviceInfo info;
        info.id = "aw610m-darfon";
        info.name = deviceLabel;
        info.type = "mouse";
        info.percentage = -1; // Deep sleep / idle
        info.charging = false;
        info.acOnline = false;
        AddUniqueBattery(result, info);
        return true;
    }

    return false;
}

static void QueryXInputBatteries(std::vector<BatteryDeviceInfo>& result) {
    HMODULE hXInput = LoadLibraryW(L"Xinput1_4.dll");
    if (!hXInput) hXInput = LoadLibraryW(L"Xinput9_1_0.dll");
    if (!hXInput) return;

    struct XInputBattInfo { BYTE BatteryType; BYTE BatteryLevel; };
    struct XInputGamepad { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger; SHORT sThumbLX; SHORT sThumbLY; SHORT sThumbRX; SHORT sThumbRY; };
    struct XInputState { DWORD dwPacketNumber; XInputGamepad Gamepad; };

    using FnGetState = DWORD(WINAPI*)(DWORD, XInputState*);
    using FnGetBattery = DWORD(WINAPI*)(DWORD, BYTE, XInputBattInfo*);

    auto pGetState = reinterpret_cast<FnGetState>(GetProcAddress(hXInput, "XInputGetState"));
    auto pGetBattery = reinterpret_cast<FnGetBattery>(GetProcAddress(hXInput, "XInputGetBatteryInformation"));

    if (pGetState && pGetBattery) {
        for (DWORD userIdx = 0; userIdx < 4; ++userIdx) {
            XInputState st{};
            if (pGetState(userIdx, &st) != ERROR_SUCCESS) continue;

            XInputBattInfo bi{};
            if (pGetBattery(userIdx, 0x00 /*GAMEPAD*/, &bi) == ERROR_SUCCESS && bi.BatteryType != 0x00) {
                int pct = -1;
                bool charging = false;
                if (bi.BatteryType == 0x01 /*WIRED*/) {
                    pct = 100;
                    charging = true;
                } else if (bi.BatteryType == 0x02 || bi.BatteryType == 0x03) {
                    switch (bi.BatteryLevel) {
                    case 0: pct = 10; break;
                    case 1: pct = 30; break;
                    case 2: pct = 65; break;
                    case 3: pct = 100; break;
                    default: pct = -1; break;
                    }
                }
                if (pct >= 0) {
                    BatteryDeviceInfo info;
                    info.id = "xinput-pad-" + std::to_string(userIdx);
                    info.name = "Xbox Controller #" + std::to_string(userIdx + 1);
                    info.type = "controller";
                    info.percentage = pct;
                    info.charging = charging;
                    info.acOnline = charging;
                    AddUniqueBattery(result, info);
                }
            }

            XInputBattInfo hi{};
            if (pGetBattery(userIdx, 0x01 /*HEADSET*/, &hi) == ERROR_SUCCESS &&
                (hi.BatteryType == 0x02 || hi.BatteryType == 0x03)) {
                int pct = -1;
                switch (hi.BatteryLevel) {
                case 0: pct = 10; break;
                case 1: pct = 30; break;
                case 2: pct = 65; break;
                case 3: pct = 100; break;
                default: break;
                }
                if (pct >= 0) {
                    BatteryDeviceInfo info;
                    info.id = "xinput-hs-" + std::to_string(userIdx);
                    info.name = "Xbox Headset #" + std::to_string(userIdx + 1);
                    info.type = "headset";
                    info.percentage = pct;
                    AddUniqueBattery(result, info);
                }
            }
        }
    }

    FreeLibrary(hXInput);
}

static void QuerySonyAndHidppBatteries(std::vector<BatteryDeviceInfo>& result) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hHidInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hHidInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hHidInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
        DWORD reqLen = 0;
        SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, nullptr, 0, &reqLen, nullptr);
        if (!reqLen) continue;
        std::vector<BYTE> detailBuf(reqLen);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devData{};
        devData.cbSize = sizeof(SP_DEVINFO_DATA);
        if (!SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, detail, reqLen, nullptr, &devData))
            continue;

        std::string pathLow = ToLowerAscii(WideToUtf8(detail->DevicePath));
        bool isSony = (pathLow.find("vid_054c") != std::string::npos);
        bool isLogiAstro = (pathLow.find("vid_046d") != std::string::npos || pathLow.find("vid_9886") != std::string::npos);
        if (!isSony && !isLogiAstro) continue;

        HANDLE hDev = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        bool canRW = (hDev != INVALID_HANDLE_VALUE);
        if (!canRW) {
            hDev = CreateFileW(detail->DevicePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (hDev == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{ sizeof(HIDD_ATTRIBUTES) };
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA prep = nullptr;
        bool hasAttrs = HidD_GetAttributes(hDev, &attrs) != FALSE;
        bool hasCaps = HidD_GetPreparsedData(hDev, &prep) && prep && (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS);
        if (prep) HidD_FreePreparsedData(prep);
        if (!hasAttrs || !hasCaps) {
            CloseHandle(hDev);
            continue;
        }

        wchar_t prodW[128]{};
        std::string devName = ResolveParentUsbName(devData.DevInst);
        if (HidD_GetProductString(hDev, prodW, sizeof(prodW)) && prodW[0] != L'\0') {
            devName = WideToUtf8(prodW);
        }

        // Sony DualSense (0x0CE6, 0x0DF2) & DualShock 4 (0x05C4, 0x09CC, 0x0BA0)
        if (attrs.VendorID == 0x054C && caps.InputReportByteLength >= 64) {
            bool isDS5 = (attrs.ProductID == 0x0CE6 || attrs.ProductID == 0x0DF2);
            bool isDS4 = (attrs.ProductID == 0x05C4 || attrs.ProductID == 0x09CC || attrs.ProductID == 0x0BA0);
            if (isDS5 || isDS4) {
                bool isBt = (caps.InputReportByteLength > 64);
                std::vector<BYTE> rpt(caps.InputReportByteLength, 0);
                if (isDS5 && isBt) {
                    std::vector<BYTE> feat(caps.FeatureReportByteLength > 0 ? caps.FeatureReportByteLength : 64, 0);
                    feat[0] = 0x05;
                    HidD_GetFeature(hDev, feat.data(), static_cast<ULONG>(feat.size()));
                    rpt[0] = 0x31;
                } else {
                    rpt[0] = isBt ? 0x11 : 0x01;
                }

                if (OverlappedReadOnce(hDev, rpt, 45)) {
                    int pct = -1;
                    bool charging = false;
                    if (isDS5) {
                        size_t off = (rpt[0] == 0x31) ? 54 : 53;
                        if (off < rpt.size()) {
                            BYTE raw = rpt[off] & 0x0F;
                            BYTE st = (rpt[off] >> 4) & 0x0F;
                            charging = (st == 0x01);
                            pct = (st == 0x02) ? 100 : std::clamp(static_cast<int>(raw) * 10 + 5, 5, 100);
                        }
                    } else {
                        size_t off = (rpt[0] == 0x11) ? 32 : 30;
                        if (off < rpt.size()) {
                            charging = (rpt[off] & 0x10) != 0;
                            BYTE raw = rpt[off] & 0x0F;
                            pct = charging ? std::min(100, static_cast<int>(raw) * 10) : std::clamp(static_cast<int>(raw) * 10 + 5, 5, 100);
                        }
                    }
                    if (pct >= 0) {
                        if (devName.empty()) devName = isDS5 ? "Sony DualSense Controller" : "Sony DualShock 4 Controller";
                        BatteryDeviceInfo info;
                        info.id = "sony-" + std::to_string(attrs.ProductID);
                        info.name = devName;
                        info.type = "controller";
                        info.percentage = pct;
                        info.charging = charging;
                        info.acOnline = !isBt;
                        AddUniqueBattery(result, info);
                    }
                }
            }
        }
        // Logitech / Astro HID++ 2.0 (VID_046D, VID_9886)
        else if ((attrs.VendorID == 0x046D || attrs.VendorID == 0x9886) &&
                 canRW && caps.UsagePage == 0xFF00 &&
                 caps.OutputReportByteLength >= 20 && caps.InputReportByteLength >= 20) {
            const BYTE devIndices[] = { 0xFF, 0x01, 0x02 };
            const WORD features[] = { 0x1004, 0x1000 };
            for (BYTE dIdx : devIndices) {
                bool foundForDev = false;
                for (WORD featId : features) {
                    std::vector<BYTE> req(caps.OutputReportByteLength, 0);
                    std::vector<BYTE> resp(caps.InputReportByteLength, 0);
                    req[0] = 0x11; req[1] = dIdx; req[2] = 0x00; req[3] = 0x0F;
                    req[4] = static_cast<BYTE>(featId >> 8);
                    req[5] = static_cast<BYTE>(featId & 0xFF);
                    if (!OverlappedWriteAndRead(hDev, req, resp, 25)) continue;
                    if (resp[0] == 0x11 && resp[1] == dIdx && resp[2] == 0x00 && resp[4] > 0) {
                        BYTE fIdx = resp[4];
                        std::fill(req.begin(), req.end(), 0);
                        std::fill(resp.begin(), resp.end(), 0);
                        req[0] = 0x11; req[1] = dIdx; req[2] = fIdx;
                        req[3] = (featId == 0x1004) ? 0x1F : 0x0F;
                        if (OverlappedWriteAndRead(hDev, req, resp, 25) && resp[0] == 0x11 && resp[1] == dIdx && resp[2] == fIdx) {
                            int pct = (resp[4] <= 100) ? static_cast<int>(resp[4]) : -1;
                            if (pct >= 0) {
                                bool charging = (featId == 0x1004) ? (resp[6] >= 1 && resp[6] <= 4) : (resp[6] == 1 || resp[6] == 3);
                                if (devName.empty()) devName = (attrs.VendorID == 0x9886) ? "Astro Wireless Headset" : "Logitech Wireless Device";
                                BatteryDeviceInfo info;
                                info.id = "hidpp-" + std::to_string(attrs.VendorID) + "-" + std::to_string(attrs.ProductID) + "-" + std::to_string(dIdx);
                                info.name = devName;
                                info.type = ClassifyDeviceType(devName);
                                info.percentage = pct;
                                info.charging = charging;
                                AddUniqueBattery(result, info);
                                foundForDev = true;
                                break;
                            }
                        }
                    }
                }
                if (foundForDev) break;
            }
        }
        CloseHandle(hDev);
    }
    SetupDiDestroyDeviceInfoList(hHidInfo);
}

std::vector<BatteryDeviceInfo> BatteryMonitor::GetAllBatteries() {
    std::vector<BatteryDeviceInfo> result;

    // 1. System / Laptop Battery
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.BatteryFlag != 128 && sps.BatteryLifePercent <= 100) {
            BatteryDeviceInfo sys;
            sys.id = "system-battery";
            sys.name = "System Battery";
            sys.type = "system";
            sys.percentage = static_cast<int>(sps.BatteryLifePercent);
            sys.charging = (sps.BatteryFlag & 8) != 0;
            sys.acOnline = (sps.ACLineStatus == 1);
            sys.lifeSeconds = (sps.BatteryLifeTime != 0xFFFFFFFF) ? static_cast<int>(sps.BatteryLifeTime) : -1;
            result.push_back(sys);
        }
    }

    // 2. Alienware AW610M Wireless / Wired Mouse telemetry (Darfon 0x0D62:0x9ABC & Primax 0x0461:0x4EC0)
    QueryAW610MBattery(result);

    // 3. Xbox Controllers & Headsets via XInput
    QueryXInputBatteries(result);

    // 4. Sony DualSense / DualShock 4 & Logitech / Astro HID++ Wireless Devices
    QuerySonyAndHidppBatteries(result);

    // 5. Windows Battery Class Devices (catches HID Battery Minidriver / BLE / Wireless Receiver batteries)
    HDEVINFO hBattInfo = SetupDiGetClassDevsW(&GUID_DEVICE_BATTERY, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hBattInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        for (DWORD bi = 0; SetupDiEnumDeviceInterfaces(hBattInfo, nullptr, &GUID_DEVICE_BATTERY, bi, &ifData); ++bi) {
            DWORD reqLen = 0;
            SetupDiGetDeviceInterfaceDetailW(hBattInfo, &ifData, nullptr, 0, &reqLen, nullptr);
            if (reqLen == 0) continue;

            std::vector<BYTE> detailBuf(reqLen);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devData{};
            devData.cbSize = sizeof(SP_DEVINFO_DATA);
            if (!SetupDiGetDeviceInterfaceDetailW(hBattInfo, &ifData, detail, reqLen, nullptr, &devData))
                continue;

            HANDLE hBatt = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hBatt == INVALID_HANDLE_VALUE) continue;

            BATTERY_QUERY_INFORMATION bqi{};
            DWORD waitZero = 0, outBytes = 0;
            if (DeviceIoControl(hBatt, IOCTL_BATTERY_QUERY_TAG, &waitZero, sizeof(waitZero),
                &bqi.BatteryTag, sizeof(bqi.BatteryTag), &outBytes, nullptr) && bqi.BatteryTag) {

                wchar_t bName[128]{};
                bqi.InformationLevel = BatteryDeviceName;
                DeviceIoControl(hBatt, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                    bName, sizeof(bName), &outBytes, nullptr);
                std::string battName = bName[0] ? WideToUtf8(bName) : ResolveParentUsbName(devData.DevInst);
                if (battName.empty()) battName = "Wireless Battery Device";

                BATTERY_INFORMATION bInfo{};
                bqi.InformationLevel = BatteryInformation;
                if (DeviceIoControl(hBatt, IOCTL_BATTERY_QUERY_INFORMATION, &bqi, sizeof(bqi),
                    &bInfo, sizeof(bInfo), &outBytes, nullptr) && bInfo.FullChargedCapacity > 0) {

                    bool isMainAcpi = ((bInfo.Capabilities & BATTERY_SYSTEM_BATTERY) != 0) ||
                        (battName.find("ACPI") != std::string::npos) ||
                        (battName.find("Control Method") != std::string::npos);

                    if (!(isMainAcpi && !result.empty())) {
                        BATTERY_WAIT_STATUS bws{};
                        bws.BatteryTag = bqi.BatteryTag;
                        BATTERY_STATUS bStatus{};
                        if (DeviceIoControl(hBatt, IOCTL_BATTERY_QUERY_STATUS, &bws, sizeof(bws),
                            &bStatus, sizeof(bStatus), &outBytes, nullptr) &&
                            bStatus.Capacity != BATTERY_UNKNOWN_CAPACITY) {

                            int pct = static_cast<int>((static_cast<ULONGLONG>(bStatus.Capacity) * 100ULL) / bInfo.FullChargedCapacity);
                            pct = std::clamp(pct, 0, 100);

                            BatteryDeviceInfo info;
                            info.id = "batt-" + std::to_string(bi);
                            info.name = battName;
                            info.type = ClassifyDeviceType(battName);
                            info.percentage = pct;
                            info.charging = (bStatus.PowerState & BATTERY_CHARGING) != 0;
                            info.acOnline = (bStatus.PowerState & BATTERY_POWER_ON_LINE) != 0;
                            AddUniqueBattery(result, info);
                        }
                    }
                }
            }
            CloseHandle(hBatt);
        }
        SetupDiDestroyDeviceInfoList(hBattInfo);
    }

    // 6. Connected Bluetooth / Wireless PnP Devices with DEVPKEY_Device_BatteryPercentage
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); ++i) {
            BYTE buf[8]{};
            DEVPROPTYPE propType = 0;
            DWORD reqSize = 0;

            if (SetupDiGetDevicePropertyW(hDevInfo, &devInfo, &PKEY_BatteryPercentage, &propType,
                buf, sizeof(buf), &reqSize, 0)) {

                DWORD batteryVal = 255;
                if (reqSize == 1) batteryVal = buf[0];
                else if (reqSize >= 4) batteryVal = *reinterpret_cast<DWORD*>(buf);

                if (batteryVal <= 100) {
                    wchar_t nameBuf[256]{};
                    DWORD nameSize = 0;
                    std::string devName;

                    if (SetupDiGetDevicePropertyW(hDevInfo, &devInfo, &DEVPKEY_Device_FriendlyName, &propType,
                        reinterpret_cast<PBYTE>(nameBuf), sizeof(nameBuf), &nameSize, 0) && nameBuf[0] != L'\0') {
                        devName = WideToUtf8(nameBuf);
                    } else if (SetupDiGetDevicePropertyW(hDevInfo, &devInfo, &DEVPKEY_Device_DeviceDesc, &propType,
                        reinterpret_cast<PBYTE>(nameBuf), sizeof(nameBuf), &nameSize, 0) && nameBuf[0] != L'\0') {
                        devName = WideToUtf8(nameBuf);
                    } else if (SetupDiGetDevicePropertyW(hDevInfo, &devInfo, &DEVPKEY_NAME, &propType,
                        reinterpret_cast<PBYTE>(nameBuf), sizeof(nameBuf), &nameSize, 0) && nameBuf[0] != L'\0') {
                        devName = WideToUtf8(nameBuf);
                    }
                    if (devName.empty() || devName.find("HID") == 0) {
                        std::string pName = ResolveParentUsbName(devInfo.DevInst);
                        if (!pName.empty()) devName = pName;
                    }
                    if (devName.empty()) devName = "Wireless Device";

                    wchar_t instBuf[256]{};
                    std::string instId = "pnp-" + std::to_string(i);
                    if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfo, instBuf, 256, nullptr)) {
                        instId = WideToUtf8(instBuf);
                    }

                    BatteryDeviceInfo info;
                    info.id = instId;
                    info.name = devName;
                    info.type = ClassifyDeviceType(devName);
                    info.percentage = static_cast<int>(batteryVal);
                    info.charging = false;
                    info.acOnline = false;
                    AddUniqueBattery(result, info);
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    // 7. HID Battery Strength / Power Device Usage (0x06/0x20, 0x85/0x64..0x68) across Input & Feature reports
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO hHidInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hHidInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hHidInfo, nullptr, &hidGuid, idx, &ifData); ++idx) {
            DWORD reqLen = 0;
            SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, nullptr, 0, &reqLen, nullptr);
            if (reqLen == 0) continue;

            std::vector<BYTE> detailBuf(reqLen);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA devData{};
            devData.cbSize = sizeof(SP_DEVINFO_DATA);
            if (!SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, detail, reqLen, nullptr, &devData))
                continue;

            HANDLE hDev = CreateFileW(detail->DevicePath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hDev == INVALID_HANDLE_VALUE) {
                hDev = CreateFileW(detail->DevicePath, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            }
            if (hDev == INVALID_HANDLE_VALUE) continue;

            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (HidD_GetPreparsedData(hDev, &preparsed) && preparsed) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                    std::string fallbackName = ResolveParentUsbName(devData.DevInst);
                    if (!TryReadHidBatteryReport(hDev, preparsed, caps, HidP_Feature, fallbackName, idx, result)) {
                        TryReadHidBatteryReport(hDev, preparsed, caps, HidP_Input, fallbackName, idx, result);
                    }
                }
                HidD_FreePreparsedData(preparsed);
            }
            CloseHandle(hDev);
        }
        SetupDiDestroyDeviceInfoList(hHidInfo);
    }

    return result;
}
