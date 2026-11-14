#include "BatteryMonitor.h"
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

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
    if (lower.find("mouse") != std::string::npos || lower.find("raton") != std::string::npos || lower.find("aw6") != std::string::npos || lower.find("aw7") != std::string::npos)
        return "mouse";
    if (lower.find("keyboard") != std::string::npos || lower.find("teclado") != std::string::npos || lower.find("aw5") != std::string::npos || lower.find("aw9") != std::string::npos)
        return "keyboard";
    if (lower.find("headset") != std::string::npos || lower.find("audio") != std::string::npos || lower.find("headphone") != std::string::npos || lower.find("aw920h") != std::string::npos)
        return "headset";
    return "peripheral";
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

    // 2. Connected Bluetooth / Wireless PnP Devices with BatteryPercentage property (Windows 10/11)
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(SP_DEVINFO_DATA);

        for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); ++i) {
            BYTE batteryVal = 0;
            DEVPROPTYPE propType = 0;
            DWORD reqSize = 0;

            if (SetupDiGetDevicePropertyW(hDevInfo, &devInfo, &PKEY_BatteryPercentage, &propType,
                &batteryVal, sizeof(batteryVal), &reqSize, 0)) {

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
                    }

                    wchar_t instBuf[256]{};
                    std::string instId = "pnp-" + std::to_string(i);
                    if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfo, instBuf, 256, nullptr)) {
                        instId = WideToUtf8(instBuf);
                    }

                    // Deduplicate by friendly name if multiple HID collections report same battery
                    bool duplicate = false;
                    for (const auto& existing : result) {
                        if (existing.name == devName && existing.percentage == static_cast<int>(batteryVal)) {
                            duplicate = true;
                            break;
                        }
                    }

                    if (!duplicate) {
                        BatteryDeviceInfo info;
                        info.id = instId;
                        info.name = devName;
                        info.type = ClassifyDeviceType(devName);
                        info.percentage = static_cast<int>(batteryVal);
                        info.charging = false;
                        info.acOnline = false;
                        result.push_back(info);
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    return result;
}
