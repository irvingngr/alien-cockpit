#pragma once

#include <string>
#include <map>
#include <vector>
#include <utility>
#include <windows.h>

struct PerformanceSnapshot {
    // CPU
    std::string cpuName;
    int cpuLogicalCores = 0;
    int cpuUtilPct = 0;
    double cpuBaseGhz = 0.0;
    double cpuCurrentGhz = 0.0;
    double cpuPowerW = -1.0;
    double cpuVoltageV = -1.0;
    int cpuTempC = -1;
    int cpuFanRpm = -1;

    // GPU
    std::string gpuName;
    std::string gpuSource;
    int gpuUtilPct = 0;
    int gpuTempC = -1;
    double gpuPowerW = -1.0;
    double gpuPowerLimitW = -1.0;
    int gpuClockMhz = -1;
    int gpuMemClockMhz = -1;
    double gpuVramUsedGb = -1.0;
    double gpuVramTotalGb = -1.0;
    int gpuFanRpm = -1;

    // Memory
    double memTotalGb = 0.0;
    double memUsedGb = 0.0;
    double memAvailGb = 0.0;
    double memCachedGb = 0.0;
    double memWiredGb = 0.0;
    double memCommitGb = 0.0;
    int memPct = 0;
    int memSpeedMhz = 0;
    std::string memType;

    // C: Drive
    double diskTotalGb = 0.0;
    double diskUsedGb = 0.0;
    double diskFreeGb = 0.0;
    int diskUsedPct = 0;
    int diskActivePct = 0;
    double diskResponseMs = 0.0;
    double diskReadKbps = 0.0;
    double diskWriteKbps = 0.0;
    int diskTempC = -1;
};

class PerformanceMonitor {
public:
    static PerformanceSnapshot Sample(
        int liveCpuPct,
        const std::map<WORD, short>& senSnap,
        const std::vector<std::pair<BYTE, std::pair<WORD, BYTE>>>& fanSnap,
        const std::vector<std::pair<WORD, std::string>>& sensorNames,
        const std::vector<std::pair<BYTE, BYTE>>& fanTypes);

    static void Shutdown();
};
