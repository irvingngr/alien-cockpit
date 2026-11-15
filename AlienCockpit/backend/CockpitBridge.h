#pragma once
#include <string>
#include <vector>
#include "SimpleJson.h"
#include "BatteryMonitor.h"

class CockpitBridge {
public:
    static std::string HandleMessage(const std::string& jsonMessage);
    static void RefreshTelemetryCache(bool scanBatteries);
    static std::vector<BatteryDeviceInfo> GetCachedBatteries();
    static std::string BuildTelemetryJson();
    static std::string BuildFullStateJson();
    static void StepAnimatedEffects();
    static void TriggerStartupIntro(int styleOverride = -1);
    static void NotifyKeyPress(unsigned long vkCode);
    static bool IsWavesModeActive();
};
