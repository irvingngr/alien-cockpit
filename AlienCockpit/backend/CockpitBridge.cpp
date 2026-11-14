#include "CockpitBridge.h"
#include "alienfx-gui.h"
#include "FXHelper.h"
#include "EventHandler.h"
#include "SysMonHelper.h"
#include "BatteryMonitor.h"
#include "StartupManager.h"
#include "MonHelper.h"
#include "ConfigFan.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern ConfigHandler* conf;
extern FXHelper* fxhl;
extern EventHandler* eve;
extern MonHelper* mon;
extern ConfigFan* fan_conf;

constexpr byte EFFECT_SUMMER = 7;

static unsigned g_prePresetProfileId = 0;
static std::string g_activePresetName;

static bool g_probeActive = false;
static bool g_introActive = false;
static int g_activeIntroStyle = 0;
static ULONGLONG g_introStartMs = 0;

struct WaterDrop {
    double x;
    double y;
    ULONGLONG startMs;
};
static std::vector<WaterDrop> g_waterDrops;
static std::mutex g_dropMutex;
static DWORD g_lastDropVk = 0;
static ULONGLONG g_lastDropTick = 0;

struct CrossfadeState {
    bool active = false;
    ULONGLONG startMs = 0;
    DWORD durationMs = 700;
    std::unordered_map<DWORD, AlienFX_SDK::Afx_action> prevColors;
};
static std::unordered_map<DWORD, AlienFX_SDK::Afx_action> g_lastSentRgb;
static CrossfadeState g_crossfade;

static std::string TrimWhitespace(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static void SeedLastSentRgbIfEmpty() {
    if (!g_lastSentRgb.empty() || !conf || !conf->activeProfile) return;
    conf->modifyProfile.lockRead();
    for (const auto& ls : conf->activeProfile->lightsets) {
        if (ls.color.empty()) continue;
        AlienFX_SDK::Afx_group* grp = conf->afx_dev.GetGroupById(ls.group);
        if (!grp) continue;
        for (const auto& gl : grp->lights) {
            AlienFX_SDK::Afx_action act = ls.color[0];
            act.type = AlienFX_SDK::AlienFX_A_Color;
            g_lastSentRgb[gl.lgh] = act;
        }
    }
    conf->modifyProfile.unlockRead();
}

static void BeginCrossfade(DWORD durationMs = 700) {
    SeedLastSentRgbIfEmpty();
    g_crossfade.prevColors = g_lastSentRgb;
    g_crossfade.startMs = GetTickCount64();
    g_crossfade.durationMs = durationMs;
    g_crossfade.active = true;
}

static std::string ColorToHex(byte r, byte g, byte b) {
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    return buf;
}

static void HexToRgb(const std::string& hex, byte& r, byte& g, byte& b) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() >= 6) {
        unsigned int val = static_cast<unsigned int>(std::strtoul(h.substr(0, 6).c_str(), nullptr, 16));
        r = static_cast<byte>((val >> 16) & 0xFF);
        g = static_cast<byte>((val >> 8) & 0xFF);
        b = static_cast<byte>(val & 0xFF);
    }
}

static byte EffectNameToType(const std::string& eff) {
    if (eff == "breathing") return AlienFX_SDK::AlienFX_A_Breathing;
    if (eff == "pulse") return AlienFX_SDK::AlienFX_A_Pulse;
    if (eff == "morph") return AlienFX_SDK::AlienFX_A_Morph;
    if (eff == "spectrum") return AlienFX_SDK::AlienFX_A_Spectrum;
    if (eff == "rainbow" || eff == "wave" || eff == "aurora") return AlienFX_SDK::AlienFX_A_Rainbow;
    if (eff == "waves" || eff == "summer") return EFFECT_SUMMER;
    return AlienFX_SDK::AlienFX_A_Color;
}

static std::string EffectTypeToName(byte t, size_t colorCount = 1) {
    switch (t) {
    case AlienFX_SDK::AlienFX_A_Breathing: return "breathing";
    case AlienFX_SDK::AlienFX_A_Pulse: return "pulse";
    case AlienFX_SDK::AlienFX_A_Morph: return "morph";
    case AlienFX_SDK::AlienFX_A_Spectrum: return "spectrum";
    case AlienFX_SDK::AlienFX_A_Rainbow: return (colorCount >= 3) ? "aurora" : "wave";
    case EFFECT_SUMMER: return "waves";
    default: return "static";
    }
}

static int GetLiveCpuPercent() {
    static ULONGLONG prevIdle = 0, prevKernel = 0, prevUser = 0;
    static int lastPct = 0;
    FILETIME ftIdle{}, ftKernel{}, ftUser{};
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) return lastPct;
    auto toU64 = [](const FILETIME& ft) -> ULONGLONG {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    ULONGLONG idle = toU64(ftIdle), kernel = toU64(ftKernel), user = toU64(ftUser);
    if (prevKernel != 0 || prevUser != 0) {
        ULONGLONG dIdle = idle - prevIdle;
        ULONGLONG dTotal = (kernel - prevKernel) + (user - prevUser);
        if (dTotal > 0 && dTotal >= dIdle) {
            lastPct = static_cast<int>(((dTotal - dIdle) * 100) / dTotal);
            if (lastPct < 0) lastPct = 0;
            if (lastPct > 100) lastPct = 100;
        }
    }
    prevIdle = idle; prevKernel = kernel; prevUser = user;
    return lastPct;
}

std::string CockpitBridge::BuildTelemetryJson() {
    SimpleJson::Value root = SimpleJson::Value::Object();
    root["type"] = "telemetry";

    int cpuPct = GetLiveCpuPercent();
    int sysBatt = -1;
    bool acOnline = true;
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps)) {
        if (sps.BatteryFlag != 128 && sps.BatteryLifePercent <= 100) {
            sysBatt = static_cast<int>(sps.BatteryLifePercent);
        }
        acOnline = (sps.ACLineStatus == 1);
    }

    bool acpiReady = (mon != nullptr && mon->acpi != nullptr && mon->acpi->isSupported);
    if (acpiReady) {
        mon->GetSensorData(true);
    }

    int liveTemp = 0;
    if (mon && !mon->senValues.empty()) {
        for (const auto& kv : mon->senValues) {
            if (kv.second > liveTemp && kv.second < 130) {
                liveTemp = static_cast<int>(kv.second);
            }
        }
    }
    if (liveTemp == 0 && fxhl && fxhl->eData.Temp > 0) {
        liveTemp = static_cast<int>(fxhl->eData.Temp);
    }

    SimpleJson::Value sys = SimpleJson::Value::Object();
    sys["cpu"] = cpuPct;
    sys["ram"] = fxhl ? static_cast<int>(fxhl->eData.RAM) : 0;
    sys["gpu"] = fxhl ? static_cast<int>(fxhl->eData.GPU) : 0;
    sys["hdd"] = fxhl ? static_cast<int>(fxhl->eData.HDD) : 0;
    sys["net"] = fxhl ? static_cast<int>(fxhl->eData.NET) : 0;
    sys["temp"] = liveTemp;
    sys["batt"] = sysBatt;
    sys["ac"] = acOnline;
    root["system"] = sys;

    // Batteries & Low-Battery Mouse Red Alert (<= 15%)
    bool mouseLowBattAlert = false;
    SimpleJson::Value batts = SimpleJson::Value::Array();
    for (const auto& b : BatteryMonitor::GetAllBatteries()) {
        bool isLowMouse = (b.type == "mouse" && b.percentage >= 0 && b.percentage <= 15 && !b.charging);
        if (isLowMouse) mouseLowBattAlert = true;
        SimpleJson::Value bo = SimpleJson::Value::Object();
        bo["id"] = b.id;
        bo["name"] = b.name;
        bo["type"] = b.type;
        bo["percentage"] = b.percentage;
        bo["charging"] = b.charging;
        bo["acOnline"] = b.acOnline;
        bo["lifeSeconds"] = b.lifeSeconds;
        bo["lowBattAlert"] = isLowMouse;
        batts.push(bo);
    }
    root["batteries"] = batts;
    if (fxhl && !g_probeActive && conf && conf->lightsOn && conf->stateOn) {
        fxhl->ApplyMouseLowBatteryRed(mouseLowBattAlert);
    }

    // Fans & Sensors
    SimpleJson::Value fanData = SimpleJson::Value::Object();
    fanData["available"] = (acpiReady && (mon->fansize > 0 || mon->sensorSize > 0 || mon->powerSize > 0));
    fanData["isAdmin"] = (IsUserAnAdmin() != FALSE);
    if (mon) {
        int uiPowerMode = (mon->fullSpeedManual || (mon->powerSize > 0 && mon->powerMode >= mon->powerSize))
            ? 3
            : static_cast<int>(mon->powerMode);
        fanData["powerMode"] = uiPowerMode;
        SimpleJson::Value sensors = SimpleJson::Value::Array();
        for (const auto& kv : mon->senValues) {
            SimpleJson::Value s = SimpleJson::Value::Object();
            s["id"] = static_cast<int>(kv.first);
            std::string sname;
            if (fan_conf && fan_conf->sensors.count(kv.first)) {
                sname = fan_conf->sensors[kv.first];
            } else if (mon->acpi) {
                for (const auto& asen : mon->acpi->sensors) {
                    if (asen.sid == kv.first && !asen.name.empty()) { sname = asen.name; break; }
                }
            }
            if (sname.empty()) sname = "Sensor " + std::to_string(kv.first);
            s["name"] = sname;
            s["temp"] = static_cast<int>(kv.second);
            s["maxTemp"] = mon->maxTemps.count(kv.first) ? static_cast<int>(mon->maxTemps[kv.first]) : static_cast<int>(kv.second);
            sensors.push(s);
        }
        fanData["sensors"] = sensors;

        SimpleJson::Value fans = SimpleJson::Value::Array();
        for (const auto& kv : mon->fanRpm) {
            SimpleJson::Value f = SimpleJson::Value::Object();
            f["id"] = static_cast<int>(kv.first);
            f["rpm"] = static_cast<int>(kv.second);
            f["percent"] = static_cast<int>(mon->GetFanPercent(kv.first));
            fans.push(f);
        }
        fanData["fans"] = fans;
    }
    root["thermals"] = fanData;

    return root.dump();
}

std::string CockpitBridge::BuildFullStateJson() {
    SimpleJson::Value root = SimpleJson::Value::Object();
    root["type"] = "state";

    if (!conf) return root.dump();

    // Settings
    SimpleJson::Value settings = SimpleJson::Value::Object();
    settings["lightsOn"] = (conf->lightsOn != 0);
    settings["dimmed"] = (conf->dimmed != 0);
    settings["brightness"] = static_cast<int>(conf->stateDimmed ? conf->dimmingPower : conf->fullPower);
    settings["gammaCorrection"] = (conf->gammaCorrection != 0);
    settings["enableProfSwitch"] = (conf->enableProfSwitch != 0);
    settings["enableEffects"] = (conf->enableEffects != 0);
    settings["fanControl"] = (conf->fanControl != 0);
    settings["offOnBattery"] = (conf->offOnBattery != 0);
    settings["dimmedBatt"] = (conf->dimmedBatt != 0);
    settings["startWindows"] = StartupManager::IsStartupEnabled();
    settings["startMinimized"] = (conf->startMinimized != 0);
    settings["alwaysAdmin"] = (conf->alwaysAdmin != 0);
    settings["startupIntro"] = (conf->startupIntro != 0);
    settings["introStyle"] = static_cast<int>(conf->introStyle);
    settings["wavesMode"] = (conf->wavesMode != 0);
    settings["isAdmin"] = (IsUserAnAdmin() != FALSE);
    {
        SimpleJson::Value favs = SimpleJson::Value::Array();
        const std::string& rawFav = conf->favoriteColors;
        size_t pos = 0;
        while (pos < rawFav.size()) {
            size_t comma = rawFav.find(',', pos);
            std::string tok = (comma == std::string::npos) ? rawFav.substr(pos) : rawFav.substr(pos, comma - pos);
            tok = TrimWhitespace(tok);
            if (!tok.empty()) favs.push(tok);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        settings["favorites"] = favs;
    }
    root["settings"] = settings;

    // Devices & Lights
    SimpleJson::Value devs = SimpleJson::Value::Array();
    for (const auto& dev : conf->afx_dev.fxdevs) {
        SimpleJson::Value d = SimpleJson::Value::Object();
        d["pid"] = static_cast<int>(dev.pid);
        d["vid"] = static_cast<int>(dev.vid);
        std::string dname = TrimWhitespace(dev.name);
        if (dname.empty()) {
            if (dev.vid == 0x187c) {
                dname = "Alienware Chassis / AW-ELC (Lid & Tron)";
            } else if (dev.vid == 0x0461 || (dev.dev && dev.dev->version == AlienFX_SDK::API_V7)) {
                dname = "Alienware Gaming Mouse";
            } else if (dev.vid == 0x0d62 || (dev.dev && dev.dev->version == AlienFX_SDK::API_V5)) {
                dname = "Alienware RGB Keyboard";
            } else {
                char pbuf[32];
                snprintf(pbuf, sizeof(pbuf), "AlienFX Device 0x%04X", dev.pid);
                dname = pbuf;
            }
        }
        d["name"] = dname;
        d["present"] = (dev.dev != nullptr);
        d["version"] = dev.dev ? dev.dev->version : -1;

        std::vector<AlienFX_SDK::Afx_light> sortedLights = dev.lights;
        std::sort(sortedLights.begin(), sortedLights.end(),
            [](const AlienFX_SDK::Afx_light& a, const AlienFX_SDK::Afx_light& b) {
                return a.lightid < b.lightid;
            });

        SimpleJson::Value lights = SimpleJson::Value::Array();
        for (const auto& l : sortedLights) {
            SimpleJson::Value lo = SimpleJson::Value::Object();
            lo["id"] = static_cast<int>(l.lightid);
            lo["name"] = l.name;
            lo["scancode"] = static_cast<int>(l.scancode);
            lo["flags"] = static_cast<int>(l.flags);
            lights.push(lo);
        }
        d["lights"] = lights;
        devs.push(d);
    }
    root["devices"] = devs;

    // Profiles
    root["activeProfileId"] = conf->activeProfile ? static_cast<int>(conf->activeProfile->id) : 0;
    root["activePreset"] = (conf->activeProfile && conf->activeProfile->id == 9999) ? g_activePresetName : std::string("");
    SimpleJson::Value profs = SimpleJson::Value::Array();
    for (const auto& p : conf->profiles) {
        SimpleJson::Value po = SimpleJson::Value::Object();
        po["id"] = static_cast<int>(p->id);
        po["name"] = p->name;
        po["isDefault"] = ((p->flags & PROF_DEFAULT) != 0);
        po["isPreset"] = (p->id == 9999);
        po["active"] = (conf->activeProfile && conf->activeProfile->id == p->id);
        po["triggerFlags"] = static_cast<int>(p->triggerFlags);

        SimpleJson::Value apps = SimpleJson::Value::Array();
        for (const auto& a : p->triggerapp) apps.push(a);
        po["triggerApps"] = apps;

        SimpleJson::Value lsets = SimpleJson::Value::Array();
        for (const auto& gs : p->lightsets) {
            SimpleJson::Value gso = SimpleJson::Value::Object();
            gso["group"] = gs.group;
            AlienFX_SDK::Afx_group* grp = conf->afx_dev.GetGroupById(gs.group);
            gso["name"] = grp ? grp->name : ("Zone " + std::to_string(gs.group));

            SimpleJson::Value glights = SimpleJson::Value::Array();
            if (grp) {
                for (const auto& gl : grp->lights) {
                    SimpleJson::Value glo = SimpleJson::Value::Object();
                    glo["did"] = static_cast<int>(gl.did);
                    glo["lid"] = static_cast<int>(gl.lid);
                    glights.push(glo);
                }
            }
            gso["lights"] = glights;

            int zb = std::clamp(static_cast<int>(gs.brightness), 5, 100);
            gso["brightness"] = zb;
            auto unscaleChan = [zb](byte v) -> byte {
                if (zb >= 100) return v;
                return static_cast<byte>(std::clamp(static_cast<int>(std::lround((v * 100.0) / zb)), 0, 255));
            };

            if (!gs.color.empty()) {
                if (gs.baseColor.br != 0) {
                    gso["color"] = ColorToHex(gs.baseColor.r, gs.baseColor.g, gs.baseColor.b);
                } else {
                    gso["color"] = ColorToHex(unscaleChan(gs.color[0].r), unscaleChan(gs.color[0].g), unscaleChan(gs.color[0].b));
                }
                gso["effect"] = EffectTypeToName(gs.color[0].type, gs.color.size());
                gso["speed"] = static_cast<int>(gs.color[0].tempo);
                if (gs.color.size() > 1)
                    gso["color2"] = ColorToHex(unscaleChan(gs.color[1].r), unscaleChan(gs.color[1].g), unscaleChan(gs.color[1].b));
                if (gs.color.size() > 2)
                    gso["color3"] = ColorToHex(unscaleChan(gs.color[2].r), unscaleChan(gs.color[2].g), unscaleChan(gs.color[2].b));
            } else {
                gso["color"] = "#00f0ff";
                gso["effect"] = "static";
                gso["speed"] = 50;
            }
            lsets.push(gso);
        }
        po["lightsets"] = lsets;
        profs.push(po);
    }
    root["profiles"] = profs;

    // Power Modes & Fan Curves
    SimpleJson::Value powerModes = SimpleJson::Value::Array();
    if (fan_conf) {
        for (const auto& kv : fan_conf->powers) {
            SimpleJson::Value pm = SimpleJson::Value::Object();
            pm["id"] = static_cast<int>(kv.first);
            pm["name"] = kv.second;
            powerModes.push(pm);
        }
    }
    root["powerModes"] = powerModes;

    SimpleJson::Value fanCurves = SimpleJson::Value::Array();
    if (fan_conf && fan_conf->lastProf) {
        for (const auto& fkv : fan_conf->lastProf->fanControls) {
            for (const auto& skv : fkv.second) {
                SimpleJson::Value fc = SimpleJson::Value::Object();
                fc["fanId"] = static_cast<int>(fkv.first);
                fc["sensorId"] = static_cast<int>(skv.first);
                fc["active"] = skv.second.active;
                SimpleJson::Value pts = SimpleJson::Value::Array();
                for (const auto& pt : skv.second.points) {
                    SimpleJson::Value pto = SimpleJson::Value::Object();
                    pto["temp"] = static_cast<int>(pt.temp);
                    pto["boost"] = static_cast<int>(pt.boost);
                    pts.push(pto);
                }
                fc["points"] = pts;
                fanCurves.push(fc);
            }
        }
    }
    root["fanCurves"] = fanCurves;

    return root.dump();
}

std::string CockpitBridge::HandleMessage(const std::string& jsonMessage) {
    SimpleJson::Value msg = SimpleJson::parse(jsonMessage);
    std::string cmd = msg.get("cmd").asString();

    if (cmd == "getState") {
        return BuildFullStateJson();
    }
    if (cmd == "getTelemetry") {
        return BuildTelemetryJson();
    }

    if (cmd == "toggleLights" && conf && fxhl) {
        conf->lightsOn = msg.get("on").asBool(true) ? 1 : 0;
        fxhl->SetState(true);
        if (conf->lightsOn) {
            fxhl->ClearAndRefresh(true);
        }
        conf->Save();
        return BuildFullStateJson();
    }

    if ((cmd == "toggleWaves" || cmd == "toggleSummer") && conf && fxhl) {
        conf->wavesMode = msg.get("enabled").asBool(false) ? 1 : 0;
        if (!conf->wavesMode) {
            std::lock_guard<std::mutex> lock(g_dropMutex);
            g_waterDrops.clear();
            BeginCrossfade(350);
            fxhl->deferV5Refresh = true;
            fxhl->ClearAndRefresh(true);
            fxhl->deferV5Refresh = false;
        }
        CockpitBridge::StepAnimatedEffects();
        conf->Save();
        return BuildFullStateJson();
    }

    if (cmd == "setBrightness" && conf && fxhl) {
        int val = msg.get("value").asInt(100);
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        conf->fullPower = static_cast<DWORD>(val);
        conf->dimmingPower = static_cast<DWORD>(val);
        fxhl->SetState(true);
        conf->Save();
        return "{\"ok\":true}";
    }

    if (cmd == "applyZoneColor" && conf && fxhl) {
        g_probeActive = false;
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        if (!pid) {
            for (const auto& d : conf->afx_dev.fxdevs) {
                if (d.present) {
                    pid = d.pid;
                    break;
                }
            }
        }
        std::string hex1 = msg.get("color").asString("#00f0ff");
        std::string hex2 = msg.get("color2").asString("#7b2cbf");
        std::string eff = msg.get("effect").asString("static");
        byte speed = static_cast<byte>(msg.get("speed").asInt(50));
        int zBright = std::clamp(msg.get("zoneBrightness").asInt(100), 5, 100);
        std::string zoneName = msg.get("zoneName").asString("Custom Zone");
        const auto& lightIds = msg.get("lightIds").arrVal;

        auto scaleChan = [zBright](byte v) -> byte {
            return static_cast<byte>(std::clamp(static_cast<int>(std::lround(v * (zBright / 100.0))), 0, 255));
        };

        byte r1Raw = 0, g1Raw = 240, b1Raw = 255;
        byte r2 = 123, g2 = 44, b2 = 191;
        HexToRgb(hex1, r1Raw, g1Raw, b1Raw);
        HexToRgb(hex2, r2, g2, b2);
        byte r1 = scaleChan(r1Raw), g1 = scaleChan(g1Raw), b1 = scaleChan(b1Raw);
        r2 = scaleChan(r2); g2 = scaleChan(g2); b2 = scaleChan(b2);

        if (!conf->activeProfile) {
            conf->activeProfile = conf->FindDefaultProfile();
        }

        if (conf->activeProfile && pid && !lightIds.empty()) {
            BeginCrossfade(700);
            conf->modifyProfile.lockWrite();

            auto isGroupShared = [&](DWORD gid) {
                int refs = 0;
                for (const auto* prof : conf->profiles) {
                    if (!prof) continue;
                    for (const auto& ls : prof->lightsets) {
                        if (ls.group == static_cast<int>(gid)) {
                            ++refs;
                            break;
                        }
                    }
                }
                return refs > 1;
            };

            auto* groupsVec = conf->afx_dev.GetGroups();
            auto allocGroupId = [&]() -> DWORD {
                DWORD maxGid = 0x10000;
                if (groupsVec) {
                    for (const auto& g : *groupsVec) {
                        if (g.gid >= maxGid) maxGid = g.gid + 1;
                    }
                }
                return maxGid;
            };

            // Remove newly-targeted (pid, lid) pairs from existing groups in active profile so they don't conflict
            for (auto it = conf->activeProfile->lightsets.begin(); it != conf->activeProfile->lightsets.end(); ) {
                AlienFX_SDK::Afx_group* existingGrp = conf->afx_dev.GetGroupById(it->group);
                if (existingGrp) {
                    bool touchesTarget = false;
                    for (const auto& gl : existingGrp->lights) {
                        if (gl.did == pid) {
                            for (const auto& lidVal : lightIds) {
                                if (gl.lid == static_cast<WORD>(lidVal.asInt(0))) {
                                    touchesTarget = true;
                                    break;
                                }
                            }
                        }
                        if (touchesTarget) break;
                    }

                    if (touchesTarget && groupsVec && isGroupShared(existingGrp->gid)) {
                        // Clone group for active profile before mutating so other profiles keep their layout
                        AlienFX_SDK::Afx_group cloned = *existingGrp;
                        cloned.gid = allocGroupId();
                        groupsVec->push_back(cloned);
                        it->group = static_cast<int>(cloned.gid);
                        existingGrp = conf->afx_dev.GetGroupById(cloned.gid);
                    }

                    if (existingGrp) {
                        for (auto lit = existingGrp->lights.begin(); lit != existingGrp->lights.end(); ) {
                            bool match = false;
                            if (lit->did == pid) {
                                for (const auto& lidVal : lightIds) {
                                    if (lit->lid == static_cast<WORD>(lidVal.asInt(0))) {
                                        match = true;
                                        break;
                                    }
                                }
                            }
                            if (match) lit = existingGrp->lights.erase(lit);
                            else ++lit;
                        }
                        if (existingGrp->lights.empty()) {
                            it = conf->activeProfile->lightsets.erase(it);
                            continue;
                        }
                    }
                }
                ++it;
            }

            // Build deduplicated light list for new/updated group
            std::vector<AlienFX_SDK::Afx_groupLight> targetLights;
            AlienFX_SDK::Afx_device* targetDev = conf->afx_dev.GetDeviceById(pid);
            bool allPowerLights = (targetDev != nullptr);
            for (const auto& lidVal : lightIds) {
                WORD lid = static_cast<WORD>(lidVal.asInt(0));
                bool dup = false;
                for (const auto& existing : targetLights) {
                    if (existing.did == pid && existing.lid == lid) { dup = true; break; }
                }
                if (!dup) {
                    AlienFX_SDK::Afx_groupLight gl{};
                    gl.did = pid;
                    gl.lid = lid;
                    targetLights.push_back(gl);
                    if (targetDev && !(conf->afx_dev.GetFlags(targetDev, lid) & ALIENFX_FLAG_POWER)) {
                        allPowerLights = false;
                    }
                }
            }
            if (allPowerLights && !targetLights.empty()) {
                // Keep hardware Power Button steady static to prevent bank reprogramming flicker
                eff = "static";
            }

            // Find or create group for this zone
            DWORD targetGid = static_cast<DWORD>(msg.get("groupId").asInt(0));
            AlienFX_SDK::Afx_group* grp = targetGid ? conf->afx_dev.GetGroupById(targetGid) : nullptr;
            if (!grp || isGroupShared(targetGid)) {
                DWORD newGid = allocGroupId();
                if (groupsVec) {
                    AlienFX_SDK::Afx_group newGrp;
                    newGrp.gid = newGid;
                    newGrp.name = zoneName;
                    newGrp.lights = targetLights;
                    groupsVec->push_back(newGrp);
                }
                targetGid = newGid;
            } else {
                grp->name = zoneName;
                grp->lights = targetLights;
            }

            // Prune orphaned groups no longer referenced by any profile
            if (groupsVec) {
                for (auto git = groupsVec->begin(); git != groupsVec->end(); ) {
                    bool referenced = (git->gid == targetGid);
                    if (!referenced) {
                        for (const auto* prof : conf->profiles) {
                            if (!prof) continue;
                            for (const auto& ls : prof->lightsets) {
                                if (ls.group == static_cast<int>(git->gid)) {
                                    referenced = true;
                                    break;
                                }
                            }
                            if (referenced) break;
                        }
                    }
                    if (!referenced && git->gid >= 0x10000) {
                        git = groupsVec->erase(git);
                    } else {
                        ++git;
                    }
                }
            }

            // Find or create groupset in active profile
            groupset* targetSet = nullptr;
            for (auto& ls : conf->activeProfile->lightsets) {
                if (ls.group == static_cast<int>(targetGid)) {
                    targetSet = &ls;
                    break;
                }
            }
            if (!targetSet) {
                groupset gs;
                gs.group = static_cast<int>(targetGid);
                conf->activeProfile->lightsets.push_back(gs);
                targetSet = &conf->activeProfile->lightsets.back();
            }

            targetSet->brightness = static_cast<byte>(zBright);
            targetSet->baseColor = AlienFX_SDK::Afx_colorcode(b1Raw, g1Raw, r1Raw, 0xff);
            targetSet->color.clear();
            AlienFX_SDK::Afx_action act1{};
            act1.type = EffectNameToType(eff);
            act1.r = r1; act1.g = g1; act1.b = b1;
            act1.tempo = speed;
            act1.time = 5;
            targetSet->color.push_back(act1);

            if (eff == "morph" || eff == "breathing" || eff == "pulse" || eff == "aurora") {
                AlienFX_SDK::Afx_action act2{};
                act2.type = act1.type;
                act2.r = r2; act2.g = g2; act2.b = b2;
                act2.tempo = speed;
                act2.time = 5;
                targetSet->color.push_back(act2);
            }
            if (eff == "aurora") {
                std::string hex3 = msg.get("color3").asString("#7b2cbf");
                byte r3 = 123, g3 = 44, b3 = 191;
                HexToRgb(hex3, r3, g3, b3);
                AlienFX_SDK::Afx_action act3{};
                act3.type = act1.type;
                act3.r = scaleChan(r3); act3.g = scaleChan(g3); act3.b = scaleChan(b3);
                act3.tempo = speed;
                act3.time = 5;
                targetSet->color.push_back(act3);
            }

            conf->modifyProfile.unlockWrite();

            fxhl->deferV5Refresh = true;
            fxhl->ClearAndRefresh(true);
            fxhl->deferV5Refresh = false;
            CockpitBridge::StepAnimatedEffects();
            conf->Save();
            conf->afx_dev.SaveMappings();
        }
        return BuildFullStateJson();
    }

    if (cmd == "addLight" && conf) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        byte lid = static_cast<byte>(msg.get("lightId").asInt(0));
        std::string name = msg.get("name").asString();
        if (name.empty()) name = "LED Zone " + std::to_string(lid);
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        if (dev) {
            bool exists = false;
            for (const auto& l : dev->lights) {
                if (l.lightid == lid) { exists = true; break; }
            }
            if (!exists) {
                AlienFX_SDK::Afx_light nl{};
                nl.lightid = lid;
                nl.name = name;
                dev->lights.push_back(nl);
                conf->afx_dev.SaveMappings();
                if (fxhl) {
                    fxhl->SetState(true);
                }
            }
        }
        return BuildFullStateJson();
    }

    if (cmd == "removeLight" && conf) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        byte lid = static_cast<byte>(msg.get("lightId").asInt(0));
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        if (dev) {
            conf->modifyProfile.lockWrite();
            for (auto lit = dev->lights.begin(); lit != dev->lights.end(); ) {
                if (lit->lightid == lid) lit = dev->lights.erase(lit);
                else ++lit;
            }
            if (auto* groupsVec = conf->afx_dev.GetGroups()) {
                for (auto& g : *groupsVec) {
                    for (auto git = g.lights.begin(); git != g.lights.end(); ) {
                        if (git->did == pid && git->lid == lid) git = g.lights.erase(git);
                        else ++git;
                    }
                }
            }
            for (auto* prof : conf->profiles) {
                if (!prof) continue;
                for (auto it = prof->lightsets.begin(); it != prof->lightsets.end(); ) {
                    AlienFX_SDK::Afx_group* grp = conf->afx_dev.GetGroupById(it->group);
                    if (!grp || grp->lights.empty()) it = prof->lightsets.erase(it);
                    else ++it;
                }
            }
            conf->modifyProfile.unlockWrite();
            conf->afx_dev.SaveMappings();
            conf->Save();
            if (fxhl && !g_probeActive) {
                fxhl->SetState(true);
                fxhl->deferV5Refresh = true;
                fxhl->ClearAndRefresh(true);
                fxhl->deferV5Refresh = false;
                CockpitBridge::StepAnimatedEffects();
            }
        }
        return BuildFullStateJson();
    }

    if (cmd == "blinkLight" && conf && fxhl) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        int lid = msg.get("lightId").asInt(-1);
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        if (dev) {
            fxhl->TestLight(dev, lid, true);
        }
        return "{\"ok\":true}";
    }

    if (cmd == "probeLed" && conf && fxhl) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        int lid = std::clamp(msg.get("lightId").asInt(0), 0, 255);
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        g_probeActive = true;
        g_introActive = false;
        if (dev) {
            fxhl->ProbeLight(dev, lid);
        }
        return "{\"ok\":true,\"probing\":true}";
    }

    if (cmd == "endProbe" && conf && fxhl) {
        g_probeActive = false;
        fxhl->oldtest = -1;
        BeginCrossfade(500);
        fxhl->SetState(true);
        fxhl->deferV5Refresh = true;
        fxhl->ClearAndRefresh(true);
        fxhl->deferV5Refresh = false;
        CockpitBridge::StepAnimatedEffects();
        return "{\"ok\":true,\"probing\":false}";
    }

    if (cmd == "testIntro") {
        int style = msg.has("style") ? msg.get("style").asInt(-1) : -1;
        CockpitBridge::TriggerStartupIntro(style);
        return "{\"ok\":true}";
    }

    if (cmd == "emergencyWhite" && conf && fxhl) {
        g_probeActive = false;
        g_introActive = false;
        bool ok = fxhl->EmergencyWhiteAll();
        int cnt = 0;
        for (const auto& d : conf->afx_dev.fxdevs) {
            if (d.present && d.dev) cnt++;
        }
        SimpleJson::Value resp = SimpleJson::Value::Object();
        resp["ok"] = ok;
        resp["count"] = cnt;
        return resp.dump();
    }

    if (cmd == "detectHardware" && conf) {
        SimpleJson::Value resp = SimpleJson::Value::Object();
        resp["ok"] = true;

        SimpleJson::Value wmi = SimpleJson::Value::Object();
        bool acpiReady = (mon != nullptr && mon->acpi != nullptr);
        wmi["isAlienware"] = (acpiReady && mon->acpi->isAlienware);
        wmi["isSupported"] = (acpiReady && mon->acpi->isSupported);
        wmi["isGmode"] = (acpiReady && mon->acpi->isGmode);
        wmi["fans"] = mon ? static_cast<int>(mon->fansize) : 0;
        wmi["sensors"] = mon ? static_cast<int>(mon->sensorSize) : 0;
        wmi["powerModes"] = mon ? static_cast<int>(mon->powerSize) : 0;
        resp["wmi"] = wmi;

        SimpleJson::Value ctrls = SimpleJson::Value::Array();
        for (const auto& e : AlienFX_SDK::Mappings::CollectHardwareDiagnostics()) {
            SimpleJson::Value co = SimpleJson::Value::Object();
            char hexBuf[16];
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", e.vid);
            co["vid"] = std::string(hexBuf);
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", e.pid);
            co["pid"] = std::string(hexBuf);
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", e.usagePage);
            co["usagePage"] = std::string(hexBuf);
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", e.usage);
            co["usage"] = std::string(hexBuf);
            co["inLen"] = static_cast<int>(e.inLen);
            co["outLen"] = static_cast<int>(e.outLen);
            co["featLen"] = static_cast<int>(e.featLen);
            co["api"] = e.detectedApi;
            co["openError"] = static_cast<int>(e.openError);
            co["product"] = e.product;
            co["path"] = e.path;
            ctrls.push(co);
        }
        resp["controllers"] = ctrls;

        SimpleJson::Value mapped = SimpleJson::Value::Array();
        for (const auto& d : conf->afx_dev.fxdevs) {
            SimpleJson::Value mo = SimpleJson::Value::Object();
            char hexBuf[16];
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", d.vid);
            mo["vid"] = std::string(hexBuf);
            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", d.pid);
            mo["pid"] = std::string(hexBuf);
            mo["name"] = d.name;
            mo["present"] = (d.present && d.dev != nullptr);
            mo["version"] = d.dev ? d.dev->version : -1;
            mo["v4MaxZones"] = d.dev ? static_cast<int>(d.dev->v4MaxZones) : 0;
            mo["lightCount"] = static_cast<int>(d.lights.size());
            mapped.push(mo);
        }
        resp["mappedDevices"] = mapped;
        return resp.dump();
    }

    if (cmd == "bindLightMapping" && conf) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        byte lid = static_cast<byte>(msg.get("lightId").asInt(0));
        std::string name = msg.get("name").asString();
        WORD scancode = static_cast<WORD>(msg.get("scancode").asInt(0));
        int flagsVal = msg.get("flags").asInt(-1);
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        if (dev) {
            bool found = false;
            for (auto& l : dev->lights) {
                if (l.lightid == lid) {
                    if (!name.empty()) l.name = name;
                    if (scancode) l.scancode = scancode;
                    if (flagsVal >= 0) l.flags = static_cast<WORD>(flagsVal);
                    found = true;
                    break;
                }
            }
            if (!found) {
                AlienFX_SDK::Afx_light nl{};
                nl.lightid = lid;
                nl.name = name.empty() ? ("LED Zone " + std::to_string(lid)) : name;
                nl.scancode = scancode;
                nl.flags = (flagsVal >= 0) ? static_cast<WORD>(flagsVal) : 0;
                dev->lights.push_back(nl);
            }
            conf->afx_dev.SaveMappings();
            conf->Save();
            if (fxhl) {
                fxhl->SetState(true);
                if (!g_probeActive) {
                    fxhl->deferV5Refresh = true;
                    fxhl->ClearAndRefresh(true);
                    fxhl->deferV5Refresh = false;
                    CockpitBridge::StepAnimatedEffects();
                }
            }
        }
        return BuildFullStateJson();
    }

    if (cmd == "activateProfile" && conf && eve && fxhl) {
        unsigned id = static_cast<unsigned>(msg.get("id").asInt(0));
        profile* p = conf->FindProfile(id);
        if (p) {
            BeginCrossfade(700);
            if (id != 9999) {
                g_activePresetName.clear();
            }
            fxhl->deferV5Refresh = true;
            eve->SwitchActiveProfile(p, true);
            if (id != 9999) {
                for (auto it = conf->profiles.begin(); it != conf->profiles.end(); ) {
                    if ((*it)->id == 9999) {
                        delete *it;
                        it = conf->profiles.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            fxhl->ClearAndRefresh(true);
            fxhl->deferV5Refresh = false;
            CockpitBridge::StepAnimatedEffects();
            conf->Save();
        }
        return BuildFullStateJson();
    }

    if (cmd == "applyGamePreset" && conf && eve && fxhl) {
        std::string presetName = msg.get("name").asString("Preset");
        std::string hex1 = msg.get("color").asString("#00f0ff");
        std::string hex2 = msg.get("color2").asString("#7b2cbf");
        std::string hex3 = msg.get("color3").asString("#7b2cbf");
        std::string eff = msg.get("effect").asString("static");
        byte speed = static_cast<byte>(std::clamp(msg.get("speed").asInt(50), 1, 100));

        byte r1 = 0, g1 = 240, b1 = 255;
        byte r2 = 123, g2 = 44, b2 = 191;
        byte r3 = 123, g3 = 44, b3 = 191;
        HexToRgb(hex1, r1, g1, b1);
        HexToRgb(hex2, r2, g2, b2);
        HexToRgb(hex3, r3, g3, b3);

        if (conf->activeProfile && conf->activeProfile->id != 9999) {
            g_prePresetProfileId = conf->activeProfile->id;
        }
        BeginCrossfade(700);
        g_activePresetName = presetName;

        conf->modifyProfile.lockWrite();
        profile* presetProf = conf->FindProfile(9999);
        if (!presetProf) {
            presetProf = new profile();
            presetProf->id = 9999;
            conf->profiles.push_back(presetProf);
        }
        presetProf->name = "Preset: " + presetName;
        presetProf->flags = 0;
        presetProf->lightsets.clear();

        auto* groupsVec = conf->afx_dev.GetGroups();
        for (const auto& dev : conf->afx_dev.fxdevs) {
            if (!dev.present || dev.lights.empty()) continue;
            std::vector<AlienFX_SDK::Afx_groupLight> targetLights;
            for (const auto& l : dev.lights) {
                if (l.flags & ALIENFX_FLAG_POWER) continue;
                AlienFX_SDK::Afx_groupLight gl{};
                gl.did = dev.pid;
                gl.lid = l.lightid;
                targetLights.push_back(gl);
            }
            if (targetLights.empty()) continue;

            DWORD gid = 0x20000 + dev.pid;
            AlienFX_SDK::Afx_group* grp = conf->afx_dev.GetGroupById(gid);
            if (!grp && groupsVec) {
                AlienFX_SDK::Afx_group ng;
                ng.gid = gid;
                ng.name = presetProf->name;
                ng.lights = targetLights;
                groupsVec->push_back(ng);
            } else if (grp) {
                grp->name = presetProf->name;
                grp->lights = targetLights;
            }

            groupset gs;
            gs.group = static_cast<int>(gid);
            AlienFX_SDK::Afx_action act1{};
            act1.type = EffectNameToType(eff);
            act1.r = r1; act1.g = g1; act1.b = b1;
            act1.tempo = speed;
            act1.time = 5;
            gs.color.push_back(act1);

            if (eff == "morph" || eff == "breathing" || eff == "pulse" || eff == "aurora") {
                AlienFX_SDK::Afx_action act2{};
                act2.type = act1.type;
                act2.r = r2; act2.g = g2; act2.b = b2;
                act2.tempo = speed;
                act2.time = 5;
                gs.color.push_back(act2);
            }
            if (eff == "aurora") {
                AlienFX_SDK::Afx_action act3{};
                act3.type = act1.type;
                act3.r = r3; act3.g = g3; act3.b = b3;
                act3.tempo = speed;
                act3.time = 5;
                gs.color.push_back(act3);
            }
            presetProf->lightsets.push_back(gs);
        }
        conf->modifyProfile.unlockWrite();

        fxhl->deferV5Refresh = true;
        eve->SwitchActiveProfile(presetProf, true);
        fxhl->ClearAndRefresh(true);
        fxhl->deferV5Refresh = false;
        CockpitBridge::StepAnimatedEffects();
        return BuildFullStateJson();
    }

    if (cmd == "restorePreset" && conf && eve && fxhl) {
        profile* target = conf->FindProfile(g_prePresetProfileId);
        if (!target || target->id == 9999) {
            target = conf->FindDefaultProfile();
        }
        if (target) {
            BeginCrossfade(700);
            g_activePresetName.clear();
            fxhl->deferV5Refresh = true;
            eve->SwitchActiveProfile(target, true);
            for (auto it = conf->profiles.begin(); it != conf->profiles.end(); ) {
                if ((*it)->id == 9999) {
                    delete *it;
                    it = conf->profiles.erase(it);
                } else {
                    ++it;
                }
            }
            fxhl->ClearAndRefresh(true);
            fxhl->deferV5Refresh = false;
            CockpitBridge::StepAnimatedEffects();
            conf->Save();
        }
        return BuildFullStateJson();
    }

    if (cmd == "createProfile" && conf) {
        std::string name = msg.get("name").asString("New Profile");
        unsigned maxId = 1;
        for (const auto& p : conf->profiles) {
            if (p->id != 9999 && p->id >= maxId) maxId = p->id + 1;
        }
        profile* np = new profile();
        np->id = maxId;
        np->name = name;
        conf->profiles.push_back(np);
        conf->Save();
        return BuildFullStateJson();
    }

    if (cmd == "deleteProfile" && conf && eve) {
        unsigned id = static_cast<unsigned>(msg.get("id").asInt(0));
        for (auto it = conf->profiles.begin(); it != conf->profiles.end(); ++it) {
            if ((*it)->id == id && !((*it)->flags & PROF_DEFAULT)) {
                if (conf->activeProfile == *it) {
                    eve->SwitchActiveProfile(conf->FindProfile(0), true);
                }
                delete *it;
                conf->profiles.erase(it);
                conf->Save();
                break;
            }
        }
        return BuildFullStateJson();
    }

    if (cmd == "setPowerMode" && mon && fan_conf) {
        int rawMode = msg.get("mode").asInt(0);
        if (rawMode >= 3) {
            mon->SetFullSpeed();
        } else {
            mon->SetPowerMode(static_cast<byte>(std::clamp(rawMode, 0, 255)));
        }
        fan_conf->Save();
        return BuildTelemetryJson();
    }

    if (cmd == "setFanCurve" && fan_conf && mon) {
        int reqFanId = msg.get("fanId").asInt(-1);
        WORD sensorId = static_cast<WORD>(msg.get("sensorId").asInt(0));
        bool active = msg.get("active").asBool(true);
        const auto& pts = msg.get("points").arrVal;

        if (sensorId == 0 || (!mon->senValues.empty() && !mon->senValues.count(sensorId))) {
            if (mon->acpi && !mon->acpi->sensors.empty()) {
                sensorId = mon->acpi->sensors.front().sid;
            } else if (!mon->senValues.empty()) {
                sensorId = mon->senValues.begin()->first;
            }
        }

        if (fan_conf->lastProf) {
            std::vector<byte> targetFans;
            if (reqFanId < 0) {
                int fc = std::max<int>(1, static_cast<int>(mon->fansize));
                for (int fi = 0; fi < fc; ++fi) {
                    targetFans.push_back(static_cast<byte>(fi));
                }
            } else {
                targetFans.push_back(static_cast<byte>(std::clamp(reqFanId, 0, 255)));
            }

            for (byte fId : targetFans) {
                for (auto& skv : fan_conf->lastProf->fanControls[fId]) {
                    if (skv.first != sensorId) {
                        skv.second.active = false;
                    }
                }
                sen_block& blk = fan_conf->lastProf->fanControls[fId][sensorId];
                blk.active = active;
                blk.points.clear();
                for (const auto& pt : pts) {
                    fan_point fp;
                    fp.temp = static_cast<byte>(std::clamp(pt.get("temp").asInt(40), 0, 120));
                    fp.boost = static_cast<byte>(std::clamp(pt.get("boost").asInt(50), 0, 100));
                    blk.points.push_back(fp);
                }
            }
            if (active && (mon->powerMode != 0 || mon->fullSpeedManual)) {
                mon->SetPowerMode(0);
            }
            fan_conf->Save();
        }
        return BuildFullStateJson();
    }

    if (cmd == "setSettings" && conf) {
        if (msg.has("alwaysAdmin")) {
            conf->alwaysAdmin = msg.get("alwaysAdmin").asBool(false) ? 1 : 0;
            if (StartupManager::IsStartupEnabled()) {
                StartupManager::SetStartupEnabled(true, conf->startMinimized != 0, conf->alwaysAdmin != 0);
            }
        }
        if (msg.has("startWindows")) {
            bool en = msg.get("startWindows").asBool(false);
            bool min = msg.has("startMinimized") ? msg.get("startMinimized").asBool(true) : (conf->startMinimized != 0);
            conf->startWindows = en ? 1 : 0;
            StartupManager::SetStartupEnabled(en, min, conf->alwaysAdmin != 0);
        }
        if (msg.has("startMinimized")) {
            conf->startMinimized = msg.get("startMinimized").asBool(false) ? 1 : 0;
            if (StartupManager::IsStartupEnabled())
                StartupManager::SetStartupEnabled(true, conf->startMinimized != 0, conf->alwaysAdmin != 0);
        }
        if (msg.has("startupIntro")) conf->startupIntro = msg.get("startupIntro").asBool(true) ? 1 : 0;
        if (msg.has("introStyle")) conf->introStyle = static_cast<DWORD>(std::clamp(msg.get("introStyle").asInt(0), 0, 1));
        if (msg.has("gammaCorrection")) conf->gammaCorrection = msg.get("gammaCorrection").asBool(true) ? 1 : 0;
        if (msg.has("enableProfSwitch")) conf->enableProfSwitch = msg.get("enableProfSwitch").asBool(true) ? 1 : 0;
        if (msg.has("enableEffects")) conf->enableEffects = msg.get("enableEffects").asBool(true) ? 1 : 0;
        if (msg.has("offOnBattery")) conf->offOnBattery = msg.get("offOnBattery").asBool(false) ? 1 : 0;
        if (msg.has("dimmedBatt")) conf->dimmedBatt = msg.get("dimmedBatt").asBool(false) ? 1 : 0;
        if (msg.has("fanControl")) {
            conf->fanControl = msg.get("fanControl").asBool(false) ? 1 : 0;
            if (eve) eve->ToggleFans();
        }
        if (msg.has("favorites")) {
            const auto& favArr = msg.get("favorites").arrVal;
            std::string joined;
            for (const auto& fv : favArr) {
                std::string c = TrimWhitespace(fv.asString());
                if (c.size() == 7 && c[0] == '#') {
                    if (!joined.empty()) joined += ",";
                    joined += c;
                }
            }
            conf->favoriteColors = joined;
        }
        conf->Save();
        if (fxhl) fxhl->Refresh(true);
        return BuildFullStateJson();
    }

    return "{\"ok\":false,\"error\":\"unknown_command\"}";
}

static void HsvToRgb(double hDeg, double s, double v, byte& r, byte& g, byte& b) {
    double h = std::fmod(hDeg, 360.0);
    if (h < 0.0) h += 360.0;
    double c = v * s;
    double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double rp = 0, gp = 0, bp = 0;
    if (h < 60.0) { rp = c; gp = x; }
    else if (h < 120.0) { rp = x; gp = c; }
    else if (h < 180.0) { gp = c; bp = x; }
    else if (h < 240.0) { gp = x; bp = c; }
    else if (h < 300.0) { rp = x; bp = c; }
    else { rp = c; bp = x; }
    r = static_cast<byte>(std::clamp((rp + m) * 255.0, 0.0, 255.0));
    g = static_cast<byte>(std::clamp((gp + m) * 255.0, 0.0, 255.0));
    b = static_cast<byte>(std::clamp((bp + m) * 255.0, 0.0, 255.0));
}

static AlienFX_SDK::Afx_action LerpColor(const AlienFX_SDK::Afx_action& a, const AlienFX_SDK::Afx_action& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    AlienFX_SDK::Afx_action out{};
    out.type = AlienFX_SDK::AlienFX_A_Color;
    out.r = static_cast<byte>(std::clamp(a.r + (b.r - a.r) * t, 0.0, 255.0));
    out.g = static_cast<byte>(std::clamp(a.g + (b.g - a.g) * t, 0.0, 255.0));
    out.b = static_cast<byte>(std::clamp(a.b + (b.b - a.b) * t, 0.0, 255.0));
    return out;
}

static void GetDarfonKeyXY(byte lid, double& nx, double& ny) {
    if (lid <= 19) { nx = lid / 19.0; ny = 0.0; }
    else if (lid <= 39) { nx = (lid - 20) / 19.0; ny = 0.2; }
    else if (lid <= 59) { nx = (lid - 40) / 19.0; ny = 0.4; }
    else if (lid <= 79) { nx = (lid - 60) / 19.0; ny = 0.6; }
    else if (lid <= 99) { nx = (lid - 80) / 19.0; ny = 0.8; }
    else if (lid == 114) { nx = 14.5 / 19.0; ny = 0.8; }
    else if (lid <= 119) { nx = (lid - 100) / 19.0; ny = 1.0; }
    else if (lid >= 133 && lid <= 135) { nx = (13.5 + (lid - 133)) / 19.0; ny = 1.0; }
    else { nx = (lid % 8) / 7.0; ny = 0.5; }
}

static void VkToKeyXY(DWORD vk, double& nx, double& ny) {
    nx = 0.42; ny = 0.52;
    switch (vk) {
    case VK_ESCAPE: nx = 0.0 / 19.0; ny = 0.0; break;
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
        nx = (1.0 + (vk - VK_F1)) / 19.0; ny = 0.0; break;
    case VK_HOME: nx = 13.0 / 19.0; ny = 0.0; break;
    case VK_END: nx = 14.0 / 19.0; ny = 0.0; break;
    case VK_DELETE: nx = 15.0 / 19.0; ny = 0.0; break;
    case VK_OEM_3: nx = 0.0 / 19.0; ny = 0.2; break;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        nx = (1.0 + (vk - '1')) / 19.0; ny = 0.2; break;
    case '0': nx = 10.0 / 19.0; ny = 0.2; break;
    case VK_OEM_MINUS: nx = 11.0 / 19.0; ny = 0.2; break;
    case VK_OEM_PLUS: nx = 12.0 / 19.0; ny = 0.2; break;
    case VK_BACK: nx = 14.0 / 19.0; ny = 0.2; break;
    case VK_NUMLOCK: nx = 16.0 / 19.0; ny = 0.2; break;
    case VK_DIVIDE: nx = 17.0 / 19.0; ny = 0.2; break;
    case VK_MULTIPLY: nx = 18.0 / 19.0; ny = 0.2; break;
    case VK_SUBTRACT: nx = 19.0 / 19.0; ny = 0.2; break;
    case VK_TAB: nx = 0.5 / 19.0; ny = 0.4; break;
    case 'Q': nx = 2.0 / 19.0; ny = 0.4; break;
    case 'W': nx = 3.0 / 19.0; ny = 0.4; break;
    case 'E': nx = 4.0 / 19.0; ny = 0.4; break;
    case 'R': nx = 5.0 / 19.0; ny = 0.4; break;
    case 'T': nx = 6.0 / 19.0; ny = 0.4; break;
    case 'Y': nx = 7.0 / 19.0; ny = 0.4; break;
    case 'U': nx = 8.0 / 19.0; ny = 0.4; break;
    case 'I': nx = 9.0 / 19.0; ny = 0.4; break;
    case 'O': nx = 10.0 / 19.0; ny = 0.4; break;
    case 'P': nx = 11.0 / 19.0; ny = 0.4; break;
    case VK_OEM_4: nx = 12.0 / 19.0; ny = 0.4; break;
    case VK_OEM_6: nx = 13.0 / 19.0; ny = 0.4; break;
    case VK_OEM_5: nx = 14.5 / 19.0; ny = 0.4; break;
    case VK_NUMPAD7: nx = 16.0 / 19.0; ny = 0.4; break;
    case VK_NUMPAD8: nx = 17.0 / 19.0; ny = 0.4; break;
    case VK_NUMPAD9: nx = 18.0 / 19.0; ny = 0.4; break;
    case VK_CAPITAL: nx = 0.5 / 19.0; ny = 0.6; break;
    case 'A': nx = 2.0 / 19.0; ny = 0.6; break;
    case 'S': nx = 3.0 / 19.0; ny = 0.6; break;
    case 'D': nx = 4.0 / 19.0; ny = 0.6; break;
    case 'F': nx = 5.0 / 19.0; ny = 0.6; break;
    case 'G': nx = 6.0 / 19.0; ny = 0.6; break;
    case 'H': nx = 7.0 / 19.0; ny = 0.6; break;
    case 'J': nx = 8.0 / 19.0; ny = 0.6; break;
    case 'K': nx = 9.0 / 19.0; ny = 0.6; break;
    case 'L': nx = 10.0 / 19.0; ny = 0.6; break;
    case VK_OEM_1: nx = 11.0 / 19.0; ny = 0.6; break;
    case VK_OEM_7: nx = 12.0 / 19.0; ny = 0.6; break;
    case VK_RETURN: nx = 14.0 / 19.0; ny = 0.6; break;
    case VK_NUMPAD4: nx = 16.0 / 19.0; ny = 0.6; break;
    case VK_NUMPAD5: nx = 17.0 / 19.0; ny = 0.6; break;
    case VK_NUMPAD6: nx = 18.0 / 19.0; ny = 0.6; break;
    case VK_ADD: nx = 19.0 / 19.0; ny = 0.5; break;
    case VK_SHIFT: case VK_LSHIFT: nx = 0.8 / 19.0; ny = 0.8; break;
    case 'Z': nx = 3.0 / 19.0; ny = 0.8; break;
    case 'X': nx = 4.0 / 19.0; ny = 0.8; break;
    case 'C': nx = 5.0 / 19.0; ny = 0.8; break;
    case 'V': nx = 6.0 / 19.0; ny = 0.8; break;
    case 'B': nx = 7.0 / 19.0; ny = 0.8; break;
    case 'N': nx = 8.0 / 19.0; ny = 0.8; break;
    case 'M': nx = 9.0 / 19.0; ny = 0.8; break;
    case VK_OEM_COMMA: nx = 10.0 / 19.0; ny = 0.8; break;
    case VK_OEM_PERIOD: nx = 11.0 / 19.0; ny = 0.8; break;
    case VK_OEM_2: nx = 12.0 / 19.0; ny = 0.8; break;
    case VK_RSHIFT: nx = 13.5 / 19.0; ny = 0.8; break;
    case VK_UP: nx = 14.5 / 19.0; ny = 0.8; break;
    case VK_NUMPAD1: nx = 16.0 / 19.0; ny = 0.8; break;
    case VK_NUMPAD2: nx = 17.0 / 19.0; ny = 0.8; break;
    case VK_NUMPAD3: nx = 18.0 / 19.0; ny = 0.8; break;
    case VK_CONTROL: case VK_LCONTROL: nx = 0.0 / 19.0; ny = 1.0; break;
    case VK_LWIN: nx = 2.0 / 19.0; ny = 1.0; break;
    case VK_MENU: case VK_LMENU: nx = 4.0 / 19.0; ny = 1.0; break;
    case VK_SPACE: nx = 6.5 / 19.0; ny = 1.0; break;
    case VK_RMENU: nx = 10.5 / 19.0; ny = 1.0; break;
    case VK_RWIN: nx = 11.5 / 19.0; ny = 1.0; break;
    case VK_RCONTROL: nx = 12.5 / 19.0; ny = 1.0; break;
    case VK_LEFT: nx = 13.5 / 19.0; ny = 1.0; break;
    case VK_DOWN: nx = 14.5 / 19.0; ny = 1.0; break;
    case VK_RIGHT: nx = 15.5 / 19.0; ny = 1.0; break;
    case VK_NUMPAD0: nx = 16.5 / 19.0; ny = 1.0; break;
    case VK_DECIMAL: nx = 18.0 / 19.0; ny = 1.0; break;
    default: break;
    }
}

void CockpitBridge::TriggerStartupIntro(int styleOverride) {
    if (!conf || !conf->lightsOn || !conf->stateOn) return;
    g_probeActive = false;
    g_activeIntroStyle = (styleOverride >= 0) ? std::clamp(styleOverride, 0, 1) : static_cast<int>(conf->introStyle);
    g_introActive = true;
    g_introStartMs = GetTickCount64();
}

void CockpitBridge::NotifyKeyPress(unsigned long vkCode) {
    ULONGLONG nowMs = GetTickCount64();
    if (vkCode == g_lastDropVk && (nowMs - g_lastDropTick) < 110) return;
    g_lastDropVk = vkCode;
    g_lastDropTick = nowMs;

    double kx = 0.5, ky = 0.5;
    VkToKeyXY(static_cast<DWORD>(vkCode), kx, ky);

    std::lock_guard<std::mutex> lock(g_dropMutex);
    g_waterDrops.erase(
        std::remove_if(g_waterDrops.begin(), g_waterDrops.end(),
            [nowMs](const WaterDrop& d) { return (nowMs - d.startMs) > 2200; }),
        g_waterDrops.end());
    if (g_waterDrops.size() >= 12) {
        g_waterDrops.erase(g_waterDrops.begin());
    }
    g_waterDrops.push_back({ kx, ky, nowMs });
}

bool CockpitBridge::IsWavesModeActive() {
    if (!conf || !conf->lightsOn || !conf->stateOn || !conf->activeProfile) return false;
    if (conf->wavesMode) return true;
    conf->modifyProfile.lockRead();
    bool active = false;
    for (const auto& ls : conf->activeProfile->lightsets) {
        if (!ls.color.empty() && ls.color[0].type == EFFECT_SUMMER) {
            active = true;
            break;
        }
    }
    conf->modifyProfile.unlockRead();
    return active;
}

static bool SampleCockpitBannerPixel(int col, int row, double scrollCol) {
    if (col < 0 || col > 19 || row < 0 || row > 4) return false;
    // 5x5 bitmap glyphs for C, O, C, K, P, I, T
    static const unsigned char kGlyphs[6][5] = {
        { 0x0E, 0x10, 0x10, 0x10, 0x0E }, // 0: C
        { 0x0E, 0x11, 0x11, 0x11, 0x0E }, // 1: O
        { 0x11, 0x12, 0x1C, 0x12, 0x11 }, // 2: K
        { 0x1E, 0x11, 0x1E, 0x10, 0x10 }, // 3: P
        { 0x1F, 0x04, 0x04, 0x04, 0x1F }, // 4: I
        { 0x1F, 0x04, 0x04, 0x04, 0x04 }  // 5: T
    };
    static const int kText[] = { 0, 1, 0, 2, 3, 4, 5 }; // C O C K P I T (7 * 6 - 1 = 41 cols)
    int relCol = static_cast<int>(std::lround(col - scrollCol));
    if (relCol < 0 || relCol >= 41) return false;
    int chIdx = relCol / 6;
    int glyphCol = relCol % 6;
    if (glyphCol >= 5) return false;
    int gId = kText[chIdx];
    unsigned char mask = kGlyphs[gId][row];
    return ((mask >> (4 - glyphCol)) & 1) != 0;
}

static int SampleAlienheadPixel(int col, int row) {
    // Returns 0 = background, 1 = head silhouette, 2 = glowing eyes
    // Centered across cols 6..12 (7 cols) x rows 0..5 (6 rows)
    int lc = col - 6;
    if (lc < 0 || lc > 6 || row < 0 || row > 5) return 0;
    static const unsigned char kHead[6][7] = {
        { 0, 1, 1, 1, 1, 1, 0 },
        { 1, 1, 1, 1, 1, 1, 1 },
        { 1, 2, 2, 1, 2, 2, 1 },
        { 1, 1, 2, 1, 2, 1, 1 },
        { 0, 1, 1, 1, 1, 1, 0 },
        { 0, 0, 1, 1, 1, 0, 0 }
    };
    return kHead[row][lc];
}

void CockpitBridge::StepAnimatedEffects() {
    if (!conf || !fxhl || !conf->lightsOn || !conf->stateOn || !conf->activeProfile) return;
    if (g_probeActive) return;

    ULONGLONG nowMs = GetTickCount64();
    double nowSec = static_cast<double>(nowMs) / 1000.0;

    if (g_introActive) {
        ULONGLONG elapsed = (nowMs >= g_introStartMs) ? (nowMs - g_introStartMs) : 0;
        double durMs = (g_activeIntroStyle == 1) ? 3800.0 : 2600.0;
        double p = static_cast<double>(elapsed) / durMs;
        if (p < 1.0) {
            bool introUpdated = false;
            for (const auto& dev : conf->afx_dev.fxdevs) {
                if (!dev.present || !dev.dev) continue;
                if (dev.dev->version != AlienFX_SDK::API_V5 &&
                    dev.dev->version != AlienFX_SDK::API_ACPI) {
                    continue;
                }
                fxhl->modifyQuery.lockRead();
                auto qIt = fxhl->devLightQuery.find(dev.pid);
                size_t qSize = (qIt != fxhl->devLightQuery.end()) ? qIt->second.lightQuery.size() : 0;
                HANDLE hEvt = (qIt != fxhl->devLightQuery.end()) ? qIt->second.haveNewElement : nullptr;
                fxhl->modifyQuery.unlockRead();
                if (qSize > 0 && hEvt) SetEvent(hEvt);
                if (qSize > 16) continue;

                for (const auto& l : dev.lights) {
                    if (l.flags & ALIENFX_FLAG_POWER) continue;
                    double nx = 0.0, ny = 0.0;
                    GetDarfonKeyXY(static_cast<byte>(l.lightid), nx, ny);

                    AlienFX_SDK::Afx_action frame{};
                    frame.type = AlienFX_SDK::AlienFX_A_Color;

                    if (g_activeIntroStyle == 1) {
                        int col = static_cast<int>(std::lround(nx * 19.0));
                        int row = static_cast<int>(std::lround(ny * 5.0));
                        if (p < 0.72) {
                            double sp = p / 0.72;
                            double scrollCol = 20.0 - sp * 61.0; // 20 -> -41
                            bool lit = SampleCockpitBannerPixel(col, row, scrollCol);
                            double scanBar = (row == 5) ? (0.35 + 0.65 * (0.5 + 0.5 * std::sin(p * 18.0 - col * 0.45))) : 0.0;
                            if (lit) {
                                double hue = 185.0 + 95.0 * (nx * 0.7 + sp * 0.3);
                                HsvToRgb(hue, 0.90, 1.0, frame.r, frame.g, frame.b);
                            } else if (scanBar > 0.0) {
                                HsvToRgb(205.0, 0.95, 0.32 * scanBar, frame.r, frame.g, frame.b);
                            } else {
                                frame.r = 2; frame.g = 8; frame.b = 20;
                            }
                        } else {
                            double ep = (p - 0.72) / 0.28;
                            double pulse = std::sin(ep * 3.14159265);
                            int cell = SampleAlienheadPixel(col, row);
                            if (cell == 2) {
                                frame.r = static_cast<byte>(std::lround(215.0 * pulse));
                                frame.g = static_cast<byte>(std::lround(252.0 * pulse));
                                frame.b = static_cast<byte>(std::lround(255.0 * pulse));
                            } else if (cell == 1) {
                                frame.r = static_cast<byte>(std::lround(36.0 * pulse));
                                frame.g = static_cast<byte>(std::lround(200.0 * pulse));
                                frame.b = static_cast<byte>(std::lround(255.0 * pulse));
                            } else {
                                double distCenter = std::hypot(nx - 0.5, ny - 0.5);
                                double ring = std::exp(-((distCenter - ep * 0.75) * (distCenter - ep * 0.75)) / 0.015);
                                frame.r = static_cast<byte>(std::lround(95.0 * ring * (1.0 - ep)));
                                frame.g = static_cast<byte>(std::lround(20.0 * ring * (1.0 - ep)));
                                frame.b = static_cast<byte>(std::lround(170.0 * ring * (1.0 - ep)));
                            }
                        }
                    } else {
                        double sweepX = (p < 0.65)
                            ? ((p / 0.65) * 1.45 - 0.2)
                            : (1.25 - ((p - 0.65) / 0.35) * 1.45);
                        double d = std::fabs((nx * 0.88 + ny * 0.22) - sweepX);
                        double beam = std::exp(-(d * d) / 0.02);
                        double tail = 0.22 * (1.0 - p) * (0.5 + 0.5 * std::sin(p * 12.0 + nx * 4.5));
                        double v = std::clamp(beam + tail, 0.0, 1.0);
                        double hue = 185.0 + 95.0 * std::sin(p * 6.2831853 + nx * 2.4);
                        HsvToRgb(hue, 0.92, v, frame.r, frame.g, frame.b);
                    }

                    DWORD lgh = MAKELONG(dev.pid, l.lightid);
                    g_lastSentRgb[lgh] = frame;
                    std::vector<AlienFX_SDK::Afx_action> acts{ frame };
                    fxhl->SetLight(lgh, &acts);
                    introUpdated = true;
                }
            }
            if (introUpdated) {
                fxhl->QueryUpdate();
            }
            return;
        }
        g_introActive = false;
        BeginCrossfade(650);
    }

    bool isFading = g_crossfade.active;
    double fadeT = 1.0;
    if (isFading) {
        ULONGLONG elapsed = (nowMs >= g_crossfade.startMs) ? (nowMs - g_crossfade.startMs) : 0;
        double rawT = (g_crossfade.durationMs > 0)
            ? std::clamp(static_cast<double>(elapsed) / static_cast<double>(g_crossfade.durationMs), 0.0, 1.0)
            : 1.0;
        double inv = 1.0 - rawT;
        fadeT = 1.0 - (inv * inv * inv);
    }

    // Snapshot active water drops outside lock so global Waves Mode stays zero-contention
    std::vector<WaterDrop> dropsSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_dropMutex);
        g_waterDrops.erase(
            std::remove_if(g_waterDrops.begin(), g_waterDrops.end(),
                [nowMs](const WaterDrop& d) { return (nowMs - d.startMs) > 2000; }),
            g_waterDrops.end());
        dropsSnapshot = g_waterDrops;
    }

    static bool s_hadActiveDrops = false;
    bool wavesEnabled = (conf->wavesMode != 0);
    bool hasDrops = wavesEnabled && !dropsSnapshot.empty();
    bool settleWaves = wavesEnabled && !hasDrops && s_hadActiveDrops;
    bool needWavePass = hasDrops || settleWaves;

    auto computeWaveEnergy = [&](double nx, double ny, double speedNorm) -> double {
        double waveEnergy = 0.0;
        for (const auto& drop : dropsSnapshot) {
            double ageSec = static_cast<double>(nowMs - drop.startMs) / 1000.0;
            if (ageSec < 0.0 || ageSec > 1.85) continue;
            double dist = std::hypot((nx - drop.x) * 1.85, (ny - drop.y));
            double radius = ageSec * (0.72 + speedNorm * 0.85);
            double env = 1.0 - std::clamp(ageSec / 1.85, 0.0, 1.0);
            env *= env;
            double ring1 = std::exp(-((dist - radius) * (dist - radius)) / 0.032);
            double ring2 = (radius > 0.28)
                ? 0.45 * std::exp(-((dist - (radius - 0.28)) * (dist - (radius - 0.28))) / 0.025)
                : 0.0;
            double splash = (ageSec < 0.25 && dist < 0.22)
                ? (1.0 - ageSec / 0.25) * (1.0 - dist / 0.22)
                : 0.0;
            waveEnergy += (ring1 + ring2 + splash) * env;
        }
        return std::clamp(waveEnergy, 0.0, 1.0);
    };

    AlienFX_SDK::Afx_action waveCrest{};
    waveCrest.type = AlienFX_SDK::AlienFX_A_Color;
    waveCrest.r = 215; waveCrest.g = 250; waveCrest.b = 255;

    bool anyUpdated = false;
    bool finalSettleFrame = (isFading && fadeT >= 1.0) || settleWaves;
    std::unordered_map<WORD, bool> devBusy;
    std::unordered_set<DWORD> updatedLights;

    conf->modifyProfile.lockRead();
    for (const auto& ls : conf->activeProfile->lightsets) {
        if (ls.color.empty()) continue;
        byte effType = ls.color[0].type;
        if (effType == AlienFX_SDK::AlienFX_A_Color && !isFading && !needWavePass) continue;

        AlienFX_SDK::Afx_group* grp = conf->afx_dev.GetGroupById(ls.group);
        if (!grp || grp->lights.empty()) continue;

        const auto& c1 = ls.color[0];
        AlienFX_SDK::Afx_action c2 = (ls.color.size() > 1) ? ls.color[1] : AlienFX_SDK::Afx_action{ 0 };
        AlienFX_SDK::Afx_action c3 = (ls.color.size() > 2) ? ls.color[2] : c1;

        double tNorm = std::clamp(static_cast<double>(c1.tempo), 1.0, 100.0) / 100.0;
        double cyclesPerSec = 0.035 + std::pow(tNorm, 1.75) * 0.65;
        double angle = nowSec * cyclesPerSec * 6.283185307179586;
        bool isAurora = (effType == AlienFX_SDK::AlienFX_A_Rainbow && ls.color.size() >= 3);

        for (const auto& gl : grp->lights) {
            AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(gl.did);
            if (!dev || !dev->present || !dev->dev) continue;
            // Drive only API_V5 / API_ACPI in software; API_V4/V6 use native hardware engines
            if (dev->dev->version != AlienFX_SDK::API_V5 &&
                dev->dev->version != AlienFX_SDK::API_ACPI) {
                continue;
            }
            // Never drive hardware Power Button indicator in software loop
            if (conf->afx_dev.GetFlags(dev, gl.lid) & ALIENFX_FLAG_POWER) {
                continue;
            }

            auto busyIt = devBusy.find(dev->pid);
            if (busyIt == devBusy.end()) {
                fxhl->modifyQuery.lockRead();
                auto qIt = fxhl->devLightQuery.find(dev->pid);
                size_t qSize = (qIt != fxhl->devLightQuery.end()) ? qIt->second.lightQuery.size() : 0;
                HANDLE hEvt = (qIt != fxhl->devLightQuery.end()) ? qIt->second.haveNewElement : nullptr;
                fxhl->modifyQuery.unlockRead();
                if (qSize > 0 && hEvt) {
                    SetEvent(hEvt);
                }
                bool skipDev = (!finalSettleFrame && qSize > 12) || (qSize > 250);
                busyIt = devBusy.emplace(dev->pid, skipDev).first;
            }
            if (busyIt->second) {
                continue;
            }

            double nx = 0.0, ny = 0.0;
            GetDarfonKeyXY(static_cast<byte>(gl.lid), nx, ny);

            AlienFX_SDK::Afx_action frame{};
            frame.type = AlienFX_SDK::AlienFX_A_Color;

            if (isAurora) {
                double posCoord = nx * 2.2 + ny * 0.8;
                double w1 = 0.5 + 0.5 * std::sin(angle + posCoord);
                double w2 = 0.5 + 0.5 * std::sin(angle * 0.67 - nx * 1.7 + ny * 1.1 + 1.9);
                AlienFX_SDK::Afx_action mix12 = LerpColor(c1, c2, w1);
                frame = LerpColor(mix12, c3, w2 * 0.65);
            } else if (effType == EFFECT_SUMMER) {
                double shimmer = 0.48 + 0.09 * (0.5 + 0.5 * std::sin(angle * 0.85 + nx * 3.1 - ny * 2.2));
                AlienFX_SDK::Afx_action waterBase{ 0 };
                waterBase.r = (c1.r > 0 || c1.g > 0 || c1.b > 0) ? c1.r : 42;
                waterBase.g = (c1.r > 0 || c1.g > 0 || c1.b > 0) ? c1.g : 196;
                waterBase.b = (c1.r > 0 || c1.g > 0 || c1.b > 0) ? c1.b : 255;
                AlienFX_SDK::Afx_action dark{ 0 };
                frame = LerpColor(dark, waterBase, shimmer);
                if (!hasDrops && !dropsSnapshot.empty()) {
                    double we = computeWaveEnergy(nx, ny, tNorm);
                    if (we > 0.002) frame = LerpColor(frame, waveCrest, we);
                }
            } else if (effType == AlienFX_SDK::AlienFX_A_Breathing) {
                double k = 0.05 + 0.95 * (0.5 - 0.5 * std::cos(angle));
                AlienFX_SDK::Afx_action dark{ 0 };
                frame = LerpColor(dark, c1, k);
            } else if (effType == AlienFX_SDK::AlienFX_A_Pulse) {
                double pw = 0.5 - 0.5 * std::cos(angle);
                double k = 0.04 + 0.96 * (pw * pw * pw);
                AlienFX_SDK::Afx_action dark{ 0 };
                frame = LerpColor(dark, c1, k);
            } else if (effType == AlienFX_SDK::AlienFX_A_Morph) {
                double k = 0.5 - 0.5 * std::cos(angle);
                frame = LerpColor(c1, c2, k);
            } else if (effType == AlienFX_SDK::AlienFX_A_Spectrum) {
                double hue = std::fmod(nowSec * cyclesPerSec * 120.0, 360.0);
                HsvToRgb(hue, 1.0, 1.0, frame.r, frame.g, frame.b);
            } else if (effType == AlienFX_SDK::AlienFX_A_Rainbow) {
                double hue = std::fmod(nowSec * cyclesPerSec * 150.0 + nx * 240.0 + ny * 35.0, 360.0);
                HsvToRgb(hue, 1.0, 1.0, frame.r, frame.g, frame.b);
            } else {
                frame = c1;
                frame.type = AlienFX_SDK::AlienFX_A_Color;
            }

            if (hasDrops) {
                double we = computeWaveEnergy(nx, ny, 0.5);
                if (we > 0.002) {
                    frame = LerpColor(frame, waveCrest, we);
                }
            }

            if (isFading && fadeT < 1.0) {
                auto pIt = g_crossfade.prevColors.find(gl.lgh);
                AlienFX_SDK::Afx_action prev = (pIt != g_crossfade.prevColors.end()) ? pIt->second : AlienFX_SDK::Afx_action{ 0 };
                frame = LerpColor(prev, frame, fadeT);
            }

            updatedLights.insert(gl.lgh);
            g_lastSentRgb[gl.lgh] = frame;
            std::vector<AlienFX_SDK::Afx_action> acts{ frame };
            fxhl->SetLight(gl.lgh, &acts);
            anyUpdated = true;
        }
    }
    conf->modifyProfile.unlockRead();

    if (needWavePass) {
        for (const auto& dev : conf->afx_dev.fxdevs) {
            if (!dev.present || !dev.dev) continue;
            if (dev.dev->version != AlienFX_SDK::API_V5 &&
                dev.dev->version != AlienFX_SDK::API_ACPI) {
                continue;
            }
            auto busyIt = devBusy.find(dev.pid);
            if (busyIt == devBusy.end()) {
                fxhl->modifyQuery.lockRead();
                auto qIt = fxhl->devLightQuery.find(dev.pid);
                size_t qSize = (qIt != fxhl->devLightQuery.end()) ? qIt->second.lightQuery.size() : 0;
                HANDLE hEvt = (qIt != fxhl->devLightQuery.end()) ? qIt->second.haveNewElement : nullptr;
                fxhl->modifyQuery.unlockRead();
                if (qSize > 0 && hEvt) SetEvent(hEvt);
                bool skipDev = (!finalSettleFrame && qSize > 12) || (qSize > 250);
                busyIt = devBusy.emplace(dev.pid, skipDev).first;
            }
            if (busyIt->second) continue;

            for (const auto& l : dev.lights) {
                if (l.flags & ALIENFX_FLAG_POWER) continue;
                DWORD lgh = MAKELONG(dev.pid, l.lightid);
                if (updatedLights.count(lgh)) continue;

                double nx = 0.0, ny = 0.0;
                GetDarfonKeyXY(static_cast<byte>(l.lightid), nx, ny);
                AlienFX_SDK::Afx_action frame{ 0 };
                frame.type = AlienFX_SDK::AlienFX_A_Color;
                if (hasDrops) {
                    double we = computeWaveEnergy(nx, ny, 0.5);
                    if (we > 0.002) {
                        frame = LerpColor(frame, waveCrest, we);
                    }
                }
                if (isFading && fadeT < 1.0) {
                    auto pIt = g_crossfade.prevColors.find(lgh);
                    AlienFX_SDK::Afx_action prev = (pIt != g_crossfade.prevColors.end()) ? pIt->second : AlienFX_SDK::Afx_action{ 0 };
                    frame = LerpColor(prev, frame, fadeT);
                }
                g_lastSentRgb[lgh] = frame;
                std::vector<AlienFX_SDK::Afx_action> acts{ frame };
                fxhl->SetLight(lgh, &acts);
                anyUpdated = true;
            }
        }
    }
    s_hadActiveDrops = hasDrops;

    if (anyUpdated) {
        fxhl->QueryUpdate();
    }
    if (isFading && fadeT >= 1.0) {
        g_crossfade.active = false;
        g_crossfade.prevColors.clear();
    }
}
