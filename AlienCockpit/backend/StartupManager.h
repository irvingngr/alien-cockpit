#pragma once

class StartupManager {
public:
    static bool IsStartupEnabled();
    static bool SetStartupEnabled(bool enable, bool minimized = true, bool asAdmin = false);
};
