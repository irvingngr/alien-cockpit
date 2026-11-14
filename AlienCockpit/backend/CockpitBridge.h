#pragma once
#include <string>
#include "SimpleJson.h"

class CockpitBridge {
public:
    static std::string HandleMessage(const std::string& jsonMessage);
    static std::string BuildTelemetryJson();
    static std::string BuildFullStateJson();
};
