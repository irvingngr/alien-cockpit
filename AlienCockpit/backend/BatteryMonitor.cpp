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

static std::string ClassifyDeviceType(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("mouse") != std::string::npos || lower.find("raton") != std::string::npos ||
        lower.find("aw6") != std::string::npos || lower.find("aw7") != std::string::npos ||
        lower.find("aw3") != std::string::npos || lower.find("pro wireless") != std::string::npos)
        return "mouse";
    if (lower.find("keyboard") != std::string::npos || lower.find("teclado") != std::string::npos ||
        lower.find("aw5") != std::string::npos || lower.find("aw9") != std::string::npos ||
        lower.find("aw4") != std::string::npos)
        return "keyboard";
    if (lower.find("headset") != std::string::npos || lower.find("audio") != std::string::npos ||
        lower.find("headphone") != std::string::npos || lower.find("aw920") != std::string::npos ||
        lower.find("aw720h") != std::string::npos)
        return "headset";
    return "peripheral";
}

static void AddUniqueBattery(std::vector<BatteryDeviceInfo>& result, const BatteryDeviceInfo& info) {
    for (const auto& existing : result) {
        if (existing.name == info.name && existing.percentage == info.percentage) {
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

    // 2. Windows Battery Class Devices (catches HID Battery Minidriver / BLE / Wireless Receiver batteries)
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

                    // Skip main ACPI control method battery if System Battery is already listed
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

    // 3. Connected Bluetooth / Wireless PnP Devices with DEVPKEY_Device_BatteryPercentage
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

    // 4. HID Battery Strength / Power Device Usage (0x06/0x20, 0x85/0x64..0x68) across Input & Feature reports
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

            // Open with 0 access first (works even for OS-exclusive HID mice/keyboards)
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
