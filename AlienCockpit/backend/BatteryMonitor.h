#pragma once
#include <string>
#include <vector>

struct BatteryDeviceInfo {
    std::string id;
    std::string name;
    std::string type; // "system", "mouse", "keyboard", "headset", "peripheral"
    int percentage = -1; // 0-100, -1 if unknown
    bool charging = false;
    bool acOnline = false;
    int lifeSeconds = -1;
};

class BatteryMonitor {
public:
    static std::vector<BatteryDeviceInfo> GetAllBatteries();
};
