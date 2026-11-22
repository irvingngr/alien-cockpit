#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PerformanceMonitor.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <powrprof.h>
#include <dxgi.h>
#include <wbemidl.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <mutex>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wbemuuid.lib")

namespace {

typedef struct _AC_PROCESSOR_POWER_INFORMATION {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} AC_PROCESSOR_POWER_INFORMATION;

// Minimal binary-stable NVML C ABI declarations (loaded dynamically from nvml.dll)
typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;
struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};
struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

constexpr nvmlReturn_t NVML_OK = 0;
constexpr int NVML_TEMP_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;

using FnNvmlInit = nvmlReturn_t(*)();
using FnNvmlShutdown = nvmlReturn_t(*)();
using FnNvmlDeviceGetCount = nvmlReturn_t(*)(unsigned int*);
using FnNvmlDeviceGetHandle = nvmlReturn_t(*)(unsigned int, nvmlDevice_t*);
using FnNvmlDeviceGetName = nvmlReturn_t(*)(nvmlDevice_t, char*, unsigned int);
using FnNvmlDeviceGetUtil = nvmlReturn_t(*)(nvmlDevice_t, nvmlUtilization_t*);
using FnNvmlDeviceGetTemp = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned int*);
using FnNvmlDeviceGetPower = nvmlReturn_t(*)(nvmlDevice_t, unsigned int*);
using FnNvmlDeviceGetPowerLimit = nvmlReturn_t(*)(nvmlDevice_t, unsigned int*);
using FnNvmlDeviceGetClock = nvmlReturn_t(*)(nvmlDevice_t, int, unsigned int*);
using FnNvmlDeviceGetMemory = nvmlReturn_t(*)(nvmlDevice_t, nvmlMemory_t*);

struct NvmlState {
    HMODULE dll = nullptr;
    bool initialized = false;
    nvmlDevice_t device = nullptr;
    std::string deviceName;

    FnNvmlInit pInit = nullptr;
    FnNvmlShutdown pShutdown = nullptr;
    FnNvmlDeviceGetCount pGetCount = nullptr;
    FnNvmlDeviceGetHandle pGetHandle = nullptr;
    FnNvmlDeviceGetName pGetName = nullptr;
    FnNvmlDeviceGetUtil pGetUtil = nullptr;
    FnNvmlDeviceGetTemp pGetTemp = nullptr;
    FnNvmlDeviceGetPower pGetPower = nullptr;
    FnNvmlDeviceGetPowerLimit pGetPowerLimit = nullptr;
    FnNvmlDeviceGetClock pGetClock = nullptr;
    FnNvmlDeviceGetMemory pGetMemory = nullptr;
};

struct PerfMonitorState {
    std::mutex mtx;
    bool initialized = false;

    std::string cpuName;
    int cpuLogicalCores = 0;
    double cpuBaseMhz = 0.0;

    std::string dxgiGpuName;
    double dxgiVramTotalGb = -1.0;

    int memSpeedMhz = 0;
    std::string memType;

    NvmlState nvml;

    PDH_HQUERY pdhQuery = nullptr;
    PDH_HCOUNTER hCpuPerf = nullptr;
    PDH_HCOUNTER hDiskTime = nullptr;
    PDH_HCOUNTER hDiskResp = nullptr;
    PDH_HCOUNTER hDiskRead = nullptr;
    PDH_HCOUNTER hDiskWrite = nullptr;
    PDH_HCOUNTER hGpuEng = nullptr;
    PDH_HCOUNTER hEsifPower = nullptr;
    bool pdhPrimed = false;

    IWbemServices* lhmSvc = nullptr;
    ULONGLONG lastLhmProbeMs = 0;
};

static PerfMonitorState g_perf;

static std::string ToLowerCopy(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

static std::string TrimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct ScopedBstr {
    BSTR ptr = nullptr;
    explicit ScopedBstr(const wchar_t* s) : ptr(s ? SysAllocString(s) : nullptr) {}
    ~ScopedBstr() { if (ptr) SysFreeString(ptr); }
    operator BSTR() const { return ptr; }
};

static void InitCpuStaticInfo() {
    char nameBuf[256]{};
    DWORD cb = sizeof(nameBuf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString",
        RRF_RT_REG_SZ, nullptr, nameBuf, &cb) == ERROR_SUCCESS) {
        g_perf.cpuName = TrimCopy(nameBuf);
    }
    if (g_perf.cpuName.empty()) {
        g_perf.cpuName = "System Processor";
    }

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    g_perf.cpuLogicalCores = static_cast<int>(std::max<DWORD>(1, si.dwNumberOfProcessors));

    std::vector<AC_PROCESSOR_POWER_INFORMATION> ppi(static_cast<size_t>(g_perf.cpuLogicalCores));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0,
        ppi.data(), static_cast<ULONG>(ppi.size() * sizeof(AC_PROCESSOR_POWER_INFORMATION))) == 0) {
        ULONG maxMhz = 0;
        for (const auto& p : ppi) {
            if (p.MaxMhz > maxMhz) maxMhz = p.MaxMhz;
        }
        g_perf.cpuBaseMhz = static_cast<double>(maxMhz);
    }
    if (g_perf.cpuBaseMhz <= 0.0) {
        DWORD mhz = 0;
        DWORD mhzSize = sizeof(mhz);
        if (RegGetValueA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "~MHz", RRF_RT_DWORD, nullptr, &mhz, &mhzSize) == ERROR_SUCCESS && mhz > 0) {
            g_perf.cpuBaseMhz = static_cast<double>(mhz);
        }
    }
}

static void InitNvml() {
    HMODULE mod = LoadLibraryW(L"nvml.dll");
    if (!mod) {
        mod = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    if (!mod) return;

    NvmlState& n = g_perf.nvml;
    n.dll = mod;
    n.pInit = reinterpret_cast<FnNvmlInit>(GetProcAddress(mod, "nvmlInit_v2"));
    if (!n.pInit) n.pInit = reinterpret_cast<FnNvmlInit>(GetProcAddress(mod, "nvmlInit"));
    n.pShutdown = reinterpret_cast<FnNvmlShutdown>(GetProcAddress(mod, "nvmlShutdown"));
    n.pGetCount = reinterpret_cast<FnNvmlDeviceGetCount>(GetProcAddress(mod, "nvmlDeviceGetCount_v2"));
    if (!n.pGetCount) n.pGetCount = reinterpret_cast<FnNvmlDeviceGetCount>(GetProcAddress(mod, "nvmlDeviceGetCount"));
    n.pGetHandle = reinterpret_cast<FnNvmlDeviceGetHandle>(GetProcAddress(mod, "nvmlDeviceGetHandleByIndex_v2"));
    if (!n.pGetHandle) n.pGetHandle = reinterpret_cast<FnNvmlDeviceGetHandle>(GetProcAddress(mod, "nvmlDeviceGetHandleByIndex"));
    n.pGetName = reinterpret_cast<FnNvmlDeviceGetName>(GetProcAddress(mod, "nvmlDeviceGetName"));
    n.pGetUtil = reinterpret_cast<FnNvmlDeviceGetUtil>(GetProcAddress(mod, "nvmlDeviceGetUtilizationRates"));
    n.pGetTemp = reinterpret_cast<FnNvmlDeviceGetTemp>(GetProcAddress(mod, "nvmlDeviceGetTemperature"));
    n.pGetPower = reinterpret_cast<FnNvmlDeviceGetPower>(GetProcAddress(mod, "nvmlDeviceGetPowerUsage"));
    n.pGetPowerLimit = reinterpret_cast<FnNvmlDeviceGetPowerLimit>(GetProcAddress(mod, "nvmlDeviceGetPowerManagementLimit"));
    n.pGetClock = reinterpret_cast<FnNvmlDeviceGetClock>(GetProcAddress(mod, "nvmlDeviceGetClockInfo"));
    n.pGetMemory = reinterpret_cast<FnNvmlDeviceGetMemory>(GetProcAddress(mod, "nvmlDeviceGetMemoryInfo"));

    if (!n.pInit || !n.pGetCount || !n.pGetHandle) {
        FreeLibrary(mod);
        n = {};
        return;
    }

    if (n.pInit() != NVML_OK) {
        FreeLibrary(mod);
        n = {};
        return;
    }
    n.initialized = true;

    unsigned int count = 0;
    if (n.pGetCount(&count) == NVML_OK && count > 0) {
        nvmlDevice_t bestDev = nullptr;
        unsigned long long bestMem = 0;
        for (unsigned int i = 0; i < count; ++i) {
            nvmlDevice_t dev = nullptr;
            if (n.pGetHandle(i, &dev) == NVML_OK && dev) {
                unsigned long long memTotal = 0;
                if (n.pGetMemory) {
                    nvmlMemory_t mi{};
                    if (n.pGetMemory(dev, &mi) == NVML_OK) memTotal = mi.total;
                }
                if (!bestDev || memTotal > bestMem) {
                    bestDev = dev;
                    bestMem = memTotal;
                }
            }
        }
        n.device = bestDev;
        if (n.device && n.pGetName) {
            char gbuf[128]{};
            if (n.pGetName(n.device, gbuf, sizeof(gbuf)) == NVML_OK) {
                n.deviceName = TrimCopy(gbuf);
            }
        }
    }
}

static void InitDxgiGpuInfo() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        return;
    }
    SIZE_T bestVram = 0;
    std::string bestName;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (bestName.empty() || desc.DedicatedVideoMemory > bestVram) {
                        bestVram = desc.DedicatedVideoMemory;
                        int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 1) {
                            std::string nm(len - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nm.data(), len - 1, nullptr, nullptr);
                            bestName = TrimCopy(nm);
                        }
                    }
                }
            }
            adapter->Release();
        }
    }
    factory->Release();
    g_perf.dxgiGpuName = bestName;
    if (bestVram > 0) {
        g_perf.dxgiVramTotalGb = static_cast<double>(bestVram) / (1024.0 * 1024.0 * 1024.0);
    }
}

static void InitRamWmiInfo() {
    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator))) || !locator) {
        return;
    }
    IWbemServices* cim = nullptr;
    if (SUCCEEDED(locator->ConnectServer(ScopedBstr(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &cim)) && cim) {
        CoSetProxyBlanket(cim, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        IEnumWbemClassObject* enumerator = nullptr;
        if (SUCCEEDED(cim->ExecQuery(ScopedBstr(L"WQL"),
            ScopedBstr(L"SELECT ConfiguredClockSpeed, Speed, SMBIOSMemoryType FROM Win32_PhysicalMemory"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator)) && enumerator) {
            IWbemClassObject* obj = nullptr;
            ULONG ret = 0;
            while (enumerator->Next(2000, 1, &obj, &ret) == S_OK && ret > 0 && obj) {
                VARIANT v{};
                VariantInit(&v);
                if (SUCCEEDED(obj->Get(L"ConfiguredClockSpeed", 0, &v, nullptr, nullptr)) && (v.vt == VT_I4 || v.vt == VT_UI4) && v.uintVal > 0) {
                    if (static_cast<int>(v.uintVal) > g_perf.memSpeedMhz) g_perf.memSpeedMhz = static_cast<int>(v.uintVal);
                }
                VariantClear(&v);
                if (g_perf.memSpeedMhz == 0 && SUCCEEDED(obj->Get(L"Speed", 0, &v, nullptr, nullptr)) && (v.vt == VT_I4 || v.vt == VT_UI4) && v.uintVal > 0) {
                    if (static_cast<int>(v.uintVal) > g_perf.memSpeedMhz) g_perf.memSpeedMhz = static_cast<int>(v.uintVal);
                }
                VariantClear(&v);
                if (g_perf.memType.empty() && SUCCEEDED(obj->Get(L"SMBIOSMemoryType", 0, &v, nullptr, nullptr)) && (v.vt == VT_I4 || v.vt == VT_UI4)) {
                    switch (v.uintVal) {
                    case 34: g_perf.memType = "DDR5"; break;
                    case 35: g_perf.memType = "LPDDR5"; break;
                    case 26: g_perf.memType = "DDR4"; break;
                    case 30: g_perf.memType = "LPDDR4"; break;
                    case 24: g_perf.memType = "DDR3"; break;
                    default: break;
                    }
                }
                VariantClear(&v);
                obj->Release();
            }
            enumerator->Release();
        }
        cim->Release();
    }
    locator->Release();
}

static void InitPdh() {
    if (PdhOpenQueryW(nullptr, 0, &g_perf.pdhQuery) != ERROR_SUCCESS || !g_perf.pdhQuery) {
        g_perf.pdhQuery = nullptr;
        return;
    }
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\Processor Information(_Total)\\% Processor Performance", 0, &g_perf.hCpuPerf);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\PhysicalDisk(_Total)\\% Disk Time", 0, &g_perf.hDiskTime);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\PhysicalDisk(_Total)\\Avg. Disk sec/Transfer", 0, &g_perf.hDiskResp);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &g_perf.hDiskRead);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &g_perf.hDiskWrite);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_perf.hGpuEng);
    PdhAddEnglishCounterW(g_perf.pdhQuery, L"\\EsifDeviceInformation(*)\\RAPL Power", 0, &g_perf.hEsifPower);
    PdhCollectQueryData(g_perf.pdhQuery);
    g_perf.pdhPrimed = true;
}

static bool ReadPdhDouble(PDH_HCOUNTER counter, double& outVal) {
    if (!counter) return false;
    PDH_FMT_COUNTERVALUE cv{};
    if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS &&
        (cv.CStatus == PDH_CSTATUS_VALID_DATA || cv.CStatus == PDH_CSTATUS_NEW_DATA)) {
        outVal = cv.doubleValue;
        return true;
    }
    return false;
}

static double ReadPdhMaxArray(PDH_HCOUNTER counter, bool sumByGpuEngine) {
    if (!counter) return -1.0;
    DWORD bufSize = 0, itemCount = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &itemCount, nullptr);
    if (st != PDH_MORE_DATA || bufSize == 0) return -1.0;

    std::vector<BYTE> buf(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &itemCount, items) != ERROR_SUCCESS) {
        return -1.0;
    }

    if (sumByGpuEngine) {
        std::map<std::wstring, double> engineTotals;
        for (DWORD i = 0; i < itemCount; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            if (!items[i].szName) continue;
            std::wstring nm(items[i].szName);
            size_t pos = nm.find(L"engtype_");
            std::wstring key = (pos != std::wstring::npos) ? nm.substr(pos) : L"default";
            engineTotals[key] += std::max(0.0, items[i].FmtValue.doubleValue);
        }
        double best = 0.0;
        for (const auto& kv : engineTotals) {
            if (kv.second > best) best = kv.second;
        }
        return std::clamp(best, 0.0, 100.0);
    }

    double best = -1.0;
    for (DWORD i = 0; i < itemCount; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        double v = items[i].FmtValue.doubleValue;
        if (v > best) best = v;
    }
    return best;
}

static void TrySampleLibreHardwareMonitor(double& cpuPowerW, double& cpuVoltV) {
    ULONGLONG now = GetTickCount64();
    if (!g_perf.lhmSvc) {
        if (g_perf.lastLhmProbeMs != 0 && (now - g_perf.lastLhmProbeMs) < 30000) return;
        g_perf.lastLhmProbeMs = now;
        IWbemLocator* locator = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&locator))) && locator) {
            IWbemServices* svc = nullptr;
            if (SUCCEEDED(locator->ConnectServer(ScopedBstr(L"ROOT\\LibreHardwareMonitor"), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &svc)) && svc) {
                CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                    RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                g_perf.lhmSvc = svc;
            }
            locator->Release();
        }
    }
    if (!g_perf.lhmSvc) return;

    IEnumWbemClassObject* en = nullptr;
    if (FAILED(g_perf.lhmSvc->ExecQuery(ScopedBstr(L"WQL"),
        ScopedBstr(L"SELECT Name, SensorType, Value FROM Sensor WHERE SensorType='Power' OR SensorType='Voltage'"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)) || !en) {
        g_perf.lhmSvc->Release();
        g_perf.lhmSvc = nullptr;
        return;
    }

    IWbemClassObject* obj = nullptr;
    ULONG ret = 0;
    while (en->Next(500, 1, &obj, &ret) == S_OK && ret > 0 && obj) {
        VARIANT vName{}, vType{}, vVal{};
        VariantInit(&vName); VariantInit(&vType); VariantInit(&vVal);
        if (SUCCEEDED(obj->Get(L"Name", 0, &vName, nullptr, nullptr)) &&
            SUCCEEDED(obj->Get(L"SensorType", 0, &vType, nullptr, nullptr)) &&
            SUCCEEDED(obj->Get(L"Value", 0, &vVal, nullptr, nullptr)) &&
            vName.vt == VT_BSTR && vType.vt == VT_BSTR && (vVal.vt == VT_R4 || vVal.vt == VT_R8)) {
            double val = (vVal.vt == VT_R4) ? static_cast<double>(vVal.fltVal) : vVal.dblVal;
            int nlen = WideCharToMultiByte(CP_UTF8, 0, vName.bstrVal, -1, nullptr, 0, nullptr, nullptr);
            std::string nm;
            if (nlen > 1) {
                nm.resize(nlen - 1);
                WideCharToMultiByte(CP_UTF8, 0, vName.bstrVal, -1, nm.data(), nlen - 1, nullptr, nullptr);
            }
            std::string low = ToLowerCopy(nm);
            if (_wcsicmp(vType.bstrVal, L"Power") == 0) {
                if (low.find("cpu package") != std::string::npos || low == "package") {
                    if (val >= 0.0 && val < 500.0) cpuPowerW = val;
                }
            } else if (_wcsicmp(vType.bstrVal, L"Voltage") == 0) {
                if (low.find("vcore") != std::string::npos || low.find("cpu core") != std::string::npos || low == "core (svi2 tfn)") {
                    if (val > 0.2 && val < 2.2) cpuVoltV = val;
                }
            }
        }
        VariantClear(&vName); VariantClear(&vType); VariantClear(&vVal);
        obj->Release();
    }
    en->Release();
}

static void EnsureInitialized() {
    if (g_perf.initialized) return;
    g_perf.initialized = true;
    InitCpuStaticInfo();
    InitNvml();
    InitDxgiGpuInfo();
    InitRamWmiInfo();
    InitPdh();
}

} // namespace

PerformanceSnapshot PerformanceMonitor::Sample(
    int liveCpuPct,
    const std::map<WORD, short>& senSnap,
    const std::vector<std::pair<BYTE, std::pair<WORD, BYTE>>>& fanSnap,
    const std::vector<std::pair<WORD, std::string>>& sensorNames,
    const std::vector<std::pair<BYTE, BYTE>>& fanTypes)
{
    std::lock_guard<std::mutex> lock(g_perf.mtx);
    EnsureInitialized();

    PerformanceSnapshot out{};
    out.cpuName = g_perf.cpuName;
    out.cpuLogicalCores = g_perf.cpuLogicalCores;
    out.cpuUtilPct = std::clamp(liveCpuPct, 0, 100);
    out.cpuBaseGhz = g_perf.cpuBaseMhz / 1000.0;
    out.memSpeedMhz = g_perf.memSpeedMhz;
    out.memType = g_perf.memType;

    // 1. Collect PDH counters
    if (g_perf.pdhQuery) {
        PdhCollectQueryData(g_perf.pdhQuery);

        double perfPct = 0.0;
        if (ReadPdhDouble(g_perf.hCpuPerf, perfPct) && perfPct > 1.0 && g_perf.cpuBaseMhz > 0.0) {
            out.cpuCurrentGhz = (g_perf.cpuBaseMhz * (perfPct / 100.0)) / 1000.0;
        }

        double diskTime = 0.0;
        if (ReadPdhDouble(g_perf.hDiskTime, diskTime)) {
            out.diskActivePct = std::clamp(static_cast<int>(std::lround(diskTime)), 0, 100);
        }
        double diskRespSec = 0.0;
        if (ReadPdhDouble(g_perf.hDiskResp, diskRespSec)) {
            out.diskResponseMs = std::max(0.0, diskRespSec * 1000.0);
        }
        double diskReadBps = 0.0;
        if (ReadPdhDouble(g_perf.hDiskRead, diskReadBps)) {
            out.diskReadKbps = std::max(0.0, diskReadBps / 1024.0);
        }
        double diskWriteBps = 0.0;
        if (ReadPdhDouble(g_perf.hDiskWrite, diskWriteBps)) {
            out.diskWriteKbps = std::max(0.0, diskWriteBps / 1024.0);
        }

        double esifPwr = ReadPdhMaxArray(g_perf.hEsifPower, false);
        if (esifPwr > 0.0) {
            out.cpuPowerW = (esifPwr > 400.0) ? (esifPwr / 1000.0) : esifPwr;
        }
    }

    // Fallback CPU clock via CallNtPowerInformation if PDH % Processor Performance wasn't ready
    if (out.cpuCurrentGhz <= 0.05 && g_perf.cpuLogicalCores > 0) {
        std::vector<AC_PROCESSOR_POWER_INFORMATION> ppi(static_cast<size_t>(g_perf.cpuLogicalCores));
        if (CallNtPowerInformation(ProcessorInformation, nullptr, 0,
            ppi.data(), static_cast<ULONG>(ppi.size() * sizeof(AC_PROCESSOR_POWER_INFORMATION))) == 0) {
            ULONG maxCur = 0;
            for (const auto& p : ppi) {
                if (p.CurrentMhz > maxCur) maxCur = p.CurrentMhz;
            }
            if (maxCur > 0) out.cpuCurrentGhz = static_cast<double>(maxCur) / 1000.0;
        }
    }

    // Optional LibreHardwareMonitor enrichment for CPU Power (W) & Core Voltage (V)
    if (out.cpuPowerW < 0.0 || out.cpuVoltageV < 0.0) {
        TrySampleLibreHardwareMonitor(out.cpuPowerW, out.cpuVoltageV);
    }

    // 2. Map ACPI Sensors -> CPU / GPU / SSD Temps
    std::map<WORD, std::string> nameMap;
    for (const auto& kv : sensorNames) nameMap[kv.first] = kv.second;

    for (const auto& kv : senSnap) {
        if (kv.second <= 0 || kv.second > 130) continue;
        BYTE sType = HIBYTE(kv.first);
        std::string low = ToLowerCopy(nameMap.count(kv.first) ? nameMap[kv.first] : "");
        if (out.cpuTempC < 0 && (low.find("cpu") != std::string::npos)) {
            out.cpuTempC = kv.second;
        } else if (out.gpuTempC < 0 && (low.find("gpu") != std::string::npos)) {
            out.gpuTempC = kv.second;
        } else if (out.diskTempC < 0 && (sType == 2 || low.find("ssd") != std::string::npos || low.find("disk") != std::string::npos)) {
            out.diskTempC = kv.second;
        }
    }
    if (out.cpuTempC < 0 && !senSnap.empty()) {
        for (const auto& kv : senSnap) {
            if (kv.second > 0 && kv.second < 130) { out.cpuTempC = kv.second; break; }
        }
    }

    // Map ACPI Fans -> CPU / GPU RPM
    std::map<BYTE, BYTE> fTypeMap;
    for (const auto& kv : fanTypes) fTypeMap[kv.first] = kv.second;
    for (const auto& f : fanSnap) {
        BYTE fid = f.first;
        WORD rpm = f.second.first;
        BYTE ft = fTypeMap.count(fid) ? fTypeMap[fid] : 0;
        if (ft == 1) {
            if (out.cpuFanRpm < 0 || rpm > out.cpuFanRpm) out.cpuFanRpm = rpm;
        } else if (ft == 6) {
            if (out.gpuFanRpm < 0 || rpm > out.gpuFanRpm) out.gpuFanRpm = rpm;
        }
    }
    if (out.cpuFanRpm < 0 && !fanSnap.empty()) out.cpuFanRpm = fanSnap[0].second.first;
    if (out.gpuFanRpm < 0 && fanSnap.size() > 1) out.gpuFanRpm = fanSnap[1].second.first;

    // 3. GPU metrics (NVML primary, DXGI + PDH fallback)
    bool gotNvml = false;
    if (g_perf.nvml.initialized && g_perf.nvml.device) {
        NvmlState& n = g_perf.nvml;
        out.gpuName = !n.deviceName.empty() ? n.deviceName : g_perf.dxgiGpuName;
        out.gpuSource = "NVML";
        gotNvml = true;

        if (n.pGetUtil) {
            nvmlUtilization_t util{};
            if (n.pGetUtil(n.device, &util) == NVML_OK) {
                out.gpuUtilPct = std::clamp(static_cast<int>(util.gpu), 0, 100);
            }
        }
        if (n.pGetTemp) {
            unsigned int t = 0;
            if (n.pGetTemp(n.device, NVML_TEMP_GPU, &t) == NVML_OK && t > 0 && t < 130) {
                out.gpuTempC = static_cast<int>(t);
            }
        }
        if (n.pGetPower) {
            unsigned int mw = 0;
            if (n.pGetPower(n.device, &mw) == NVML_OK) {
                out.gpuPowerW = static_cast<double>(mw) / 1000.0;
            }
        }
        if (n.pGetPowerLimit) {
            unsigned int mwLim = 0;
            if (n.pGetPowerLimit(n.device, &mwLim) == NVML_OK) {
                out.gpuPowerLimitW = static_cast<double>(mwLim) / 1000.0;
            }
        }
        if (n.pGetClock) {
            unsigned int clk = 0;
            if (n.pGetClock(n.device, NVML_CLOCK_GRAPHICS, &clk) == NVML_OK) {
                out.gpuClockMhz = static_cast<int>(clk);
            }
            unsigned int mclk = 0;
            if (n.pGetClock(n.device, NVML_CLOCK_MEM, &mclk) == NVML_OK) {
                out.gpuMemClockMhz = static_cast<int>(mclk);
            }
        }
        if (n.pGetMemory) {
            nvmlMemory_t mi{};
            if (n.pGetMemory(n.device, &mi) == NVML_OK && mi.total > 0) {
                out.gpuVramTotalGb = static_cast<double>(mi.total) / (1024.0 * 1024.0 * 1024.0);
                out.gpuVramUsedGb = static_cast<double>(mi.used) / (1024.0 * 1024.0 * 1024.0);
            }
        }
    }

    if (!gotNvml) {
        out.gpuName = !g_perf.dxgiGpuName.empty() ? g_perf.dxgiGpuName : "Graphics Adapter";
        out.gpuSource = "DXGI+PDH";
        out.gpuVramTotalGb = g_perf.dxgiVramTotalGb;
        if (g_perf.hGpuEng) {
            double gp = ReadPdhMaxArray(g_perf.hGpuEng, true);
            if (gp >= 0.0) out.gpuUtilPct = std::clamp(static_cast<int>(std::lround(gp)), 0, 100);
        }
    }

    // 4. Memory metrics
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        out.memTotalGb = static_cast<double>(ms.ullTotalPhys) / kGiB;
        out.memAvailGb = static_cast<double>(ms.ullAvailPhys) / kGiB;
        out.memUsedGb = std::max(0.0, out.memTotalGb - out.memAvailGb);
        out.memPct = static_cast<int>(std::clamp<DWORD>(ms.dwMemoryLoad, 0, 100));
    }
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi)) && pi.PageSize > 0) {
        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        double ps = static_cast<double>(pi.PageSize);
        out.memCachedGb = (static_cast<double>(pi.SystemCache) * ps) / kGiB;
        out.memWiredGb = (static_cast<double>(pi.KernelTotal) * ps) / kGiB;
        out.memCommitGb = (static_cast<double>(pi.CommitTotal) * ps) / kGiB;
    }

    // 5. C: Drive metrics
    ULARGE_INTEGER freeBytes{}, totalBytes{}, totalFreeBytes{};
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, &totalFreeBytes) && totalBytes.QuadPart > 0) {
        constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
        out.diskTotalGb = static_cast<double>(totalBytes.QuadPart) / kGiB;
        out.diskFreeGb = static_cast<double>(totalFreeBytes.QuadPart) / kGiB;
        out.diskUsedGb = std::max(0.0, out.diskTotalGb - out.diskFreeGb);
        out.diskUsedPct = std::clamp(static_cast<int>(std::lround((out.diskUsedGb * 100.0) / out.diskTotalGb)), 0, 100);
    }

    return out;
}

void PerformanceMonitor::Shutdown() {
    std::lock_guard<std::mutex> lock(g_perf.mtx);
    if (g_perf.pdhQuery) {
        PdhCloseQuery(g_perf.pdhQuery);
        g_perf.pdhQuery = nullptr;
    }
    if (g_perf.lhmSvc) {
        g_perf.lhmSvc->Release();
        g_perf.lhmSvc = nullptr;
    }
    if (g_perf.nvml.initialized && g_perf.nvml.pShutdown) {
        g_perf.nvml.pShutdown();
    }
    if (g_perf.nvml.dll) {
        FreeLibrary(g_perf.nvml.dll);
    }
    g_perf.nvml = {};
    g_perf.initialized = false;
}
