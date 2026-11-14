#include "BatteryMonitor.h"
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <algorithm>
#include <cctype>

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

    // 2. Connected Bluetooth / Wireless PnP Devices with DEVPKEY_Device_BatteryPercentage
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
                    std::string devName = "Wireless Device";

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

    // 3. HID Battery Strength / Power Device Usage (0x06/0x20, 0x84/0x65, 0x85/0x66) for 2.4GHz / USB wireless peripherals
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
            if (!SetupDiGetDeviceInterfaceDetailW(hHidInfo, &ifData, detail, reqLen, nullptr, nullptr))
                continue;

            HANDLE hDev = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hDev == INVALID_HANDLE_VALUE) {
                hDev = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            }
            if (hDev == INVALID_HANDLE_VALUE) continue;

            PHIDP_PREPARSED_DATA preparsed = nullptr;
            if (HidD_GetPreparsedData(hDev, &preparsed) && preparsed) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.FeatureReportByteLength > 0) {
                    USHORT numCaps = caps.NumberFeatureValueCaps;
                    if (numCaps > 0) {
                        std::vector<HIDP_VALUE_CAPS> vCaps(numCaps);
                        if (HidP_GetValueCaps(HidP_Feature, vCaps.data(), &numCaps, preparsed) == HIDP_STATUS_SUCCESS) {
                            for (USHORT c = 0; c < numCaps; ++c) {
                                bool isBatt = (vCaps[c].UsagePage == 0x06 && vCaps[c].NotRange.Usage == 0x20) ||
                                              (vCaps[c].UsagePage == 0x85 && (vCaps[c].NotRange.Usage == 0x65 || vCaps[c].NotRange.Usage == 0x66));
                                if (isBatt) {
                                    std::vector<BYTE> report(caps.FeatureReportByteLength, 0);
                                    report[0] = vCaps[c].ReportID;
                                    if (HidD_GetFeature(hDev, report.data(), static_cast<ULONG>(report.size()))) {
                                        ULONG val = 0;
                                        if (HidP_GetUsageValue(HidP_Feature, vCaps[c].UsagePage, vCaps[c].LinkCollection,
                                            vCaps[c].NotRange.Usage, &val, preparsed,
                                            reinterpret_cast<PCHAR>(report.data()), static_cast<ULONG>(report.size())) == HIDP_STATUS_SUCCESS) {
                                            if (val <= 100) {
                                                wchar_t prod[128]{};
                                                std::string pname = "HID Wireless Device";
                                                if (HidD_GetProductString(hDev, prod, sizeof(prod)) && prod[0] != L'\0') {
                                                    pname = WideToUtf8(prod);
                                                }
                                                BatteryDeviceInfo info;
                                                info.id = "hid-" + std::to_string(idx);
                                                info.name = pname;
                                                info.type = ClassifyDeviceType(pname);
                                                info.percentage = static_cast<int>(val);
                                                info.charging = false;
                                                info.acOnline = false;
                                                AddUniqueBattery(result, info);
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
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
