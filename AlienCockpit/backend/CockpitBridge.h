#pragma once
#include <string>
#include "SimpleJson.h"

class CockpitBridge {
public:
    static std::string HandleMessage(const std::string& jsonMessage);
    static std::string BuildTelemetryJson();
    static std::string BuildFullStateJson();
    static void StepAnimatedEffects();
    static void TriggerStartupIntro();
    static void NotifyKeyPress(unsigned long vkCode);
    static bool IsSummerEffectActive();
};
