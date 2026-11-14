#include "CockpitBridge.h"
#include "alienfx-gui.h"
#include "FXHelper.h"
#include "EventHandler.h"
#include "SysMonHelper.h"
#include "BatteryMonitor.h"
#include "StartupManager.h"
#include "MonHelper.h"
#include "ConfigFan.h"
#include <cstdio>
#include <cstdlib>

extern ConfigHandler* conf;
extern FXHelper* fxhl;
extern EventHandler* eve;
extern MonHelper* mon;
extern ConfigFan* fan_conf;

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
    if (eff == "rainbow" || eff == "wave") return AlienFX_SDK::AlienFX_A_Rainbow;
    return AlienFX_SDK::AlienFX_A_Color;
}

static std::string EffectTypeToName(byte t) {
    switch (t) {
    case AlienFX_SDK::AlienFX_A_Breathing: return "breathing";
    case AlienFX_SDK::AlienFX_A_Pulse: return "pulse";
    case AlienFX_SDK::AlienFX_A_Morph: return "morph";
    case AlienFX_SDK::AlienFX_A_Spectrum: return "spectrum";
    case AlienFX_SDK::AlienFX_A_Rainbow: return "wave";
    default: return "static";
    }
}

std::string CockpitBridge::BuildTelemetryJson() {
    SimpleJson::Value root = SimpleJson::Value::Object();
    root["type"] = "telemetry";

    SimpleJson::Value sys = SimpleJson::Value::Object();
    if (fxhl) {
        sys["cpu"] = static_cast<int>(fxhl->eData.CPU);
        sys["ram"] = static_cast<int>(fxhl->eData.RAM);
        sys["gpu"] = static_cast<int>(fxhl->eData.GPU);
        sys["hdd"] = static_cast<int>(fxhl->eData.HDD);
        sys["net"] = static_cast<int>(fxhl->eData.NET);
        sys["temp"] = static_cast<int>(fxhl->eData.Temp);
        sys["batt"] = static_cast<int>(fxhl->eData.Batt);
        sys["ac"] = (fxhl->eData.ACP == 1);
    }
    root["system"] = sys;

    // Batteries
    SimpleJson::Value batts = SimpleJson::Value::Array();
    for (const auto& b : BatteryMonitor::GetAllBatteries()) {
        SimpleJson::Value bo = SimpleJson::Value::Object();
        bo["id"] = b.id;
        bo["name"] = b.name;
        bo["type"] = b.type;
        bo["percentage"] = b.percentage;
        bo["charging"] = b.charging;
        bo["acOnline"] = b.acOnline;
        bo["lifeSeconds"] = b.lifeSeconds;
        batts.push(bo);
    }
    root["batteries"] = batts;

    // Fans & Sensors
    SimpleJson::Value fanData = SimpleJson::Value::Object();
    fanData["available"] = (mon != nullptr && mon->acpi != nullptr);
    if (mon) {
        fanData["powerMode"] = static_cast<int>(mon->powerMode);
        SimpleJson::Value sensors = SimpleJson::Value::Array();
        for (const auto& kv : mon->senValues) {
            SimpleJson::Value s = SimpleJson::Value::Object();
            s["id"] = static_cast<int>(kv.first);
            std::string sname = (fan_conf && fan_conf->sensors.count(kv.first)) ? fan_conf->sensors[kv.first] : ("Sensor " + std::to_string(kv.first));
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
    settings["brightness"] = static_cast<int>(conf->dimmingPower);
    settings["gammaCorrection"] = (conf->gammaCorrection != 0);
    settings["enableProfSwitch"] = (conf->enableProfSwitch != 0);
    settings["enableEffects"] = (conf->enableEffects != 0);
    settings["fanControl"] = (conf->fanControl != 0);
    settings["offOnBattery"] = (conf->offOnBattery != 0);
    settings["dimmedBatt"] = (conf->dimmedBatt != 0);
    settings["startWindows"] = StartupManager::IsStartupEnabled();
    settings["startMinimized"] = (conf->startMinimized != 0);
    root["settings"] = settings;

    // Devices & Lights
    SimpleJson::Value devs = SimpleJson::Value::Array();
    for (const auto& dev : conf->afx_dev.fxdevs) {
        SimpleJson::Value d = SimpleJson::Value::Object();
        d["pid"] = static_cast<int>(dev.pid);
        d["vid"] = static_cast<int>(dev.vid);
        d["name"] = dev.name.empty() ? ("AlienFX Device 0x" + std::to_string(dev.pid)) : dev.name;
        d["present"] = (dev.dev != nullptr);
        d["version"] = dev.dev ? dev.dev->version : -1;

        SimpleJson::Value lights = SimpleJson::Value::Array();
        for (const auto& l : dev.lights) {
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
    SimpleJson::Value profs = SimpleJson::Value::Array();
    for (const auto& p : conf->profiles) {
        SimpleJson::Value po = SimpleJson::Value::Object();
        po["id"] = static_cast<int>(p->id);
        po["name"] = p->name;
        po["isDefault"] = ((p->flags & PROF_DEFAULT) != 0);
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

            if (!gs.color.empty()) {
                gso["color"] = ColorToHex(gs.color[0].r, gs.color[0].g, gs.color[0].b);
                gso["effect"] = EffectTypeToName(gs.color[0].type);
                gso["speed"] = static_cast<int>(gs.color[0].tempo);
                if (gs.color.size() > 1)
                    gso["color2"] = ColorToHex(gs.color[1].r, gs.color[1].g, gs.color[1].b);
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
        fxhl->SetState();
        conf->Save();
        return BuildFullStateJson();
    }

    if (cmd == "setBrightness" && conf && fxhl) {
        int val = msg.get("value").asInt(100);
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        conf->dimmingPower = static_cast<DWORD>(val);
        fxhl->SetState(true);
        conf->Save();
        return "{\"ok\":true}";
    }

    if (cmd == "applyZoneColor" && conf && fxhl) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        std::string hex1 = msg.get("color").asString("#00f0ff");
        std::string hex2 = msg.get("color2").asString("#7b2cbf");
        std::string eff = msg.get("effect").asString("static");
        byte speed = static_cast<byte>(msg.get("speed").asInt(50));
        std::string zoneName = msg.get("zoneName").asString("Custom Zone");
        const auto& lightIds = msg.get("lightIds").arrVal;

        byte r1 = 0, g1 = 240, b1 = 255;
        byte r2 = 123, g2 = 44, b2 = 191;
        HexToRgb(hex1, r1, g1, b1);
        HexToRgb(hex2, r2, g2, b2);

        if (conf->activeProfile && !lightIds.empty()) {
            // Find or create group for this zone
            DWORD targetGid = static_cast<DWORD>(msg.get("groupId").asInt(0));
            AlienFX_SDK::Afx_group* grp = targetGid ? conf->afx_dev.GetGroupById(targetGid) : nullptr;
            if (!grp) {
                // Create a new group
                DWORD maxGid = 0x10000;
                for (const auto& g : conf->afx_dev.GetGroups()) {
                    if (g.gid >= maxGid) maxGid = g.gid + 1;
                }
                AlienFX_SDK::Afx_group newGrp;
                newGrp.gid = maxGid;
                newGrp.name = zoneName;
                for (const auto& lidVal : lightIds) {
                    AlienFX_SDK::Afx_groupLight gl;
                    gl.did = pid;
                    gl.lid = static_cast<WORD>(lidVal.asInt(0));
                    newGrp.lights.push_back(gl);
                }
                conf->afx_dev.GetGroups().push_back(newGrp);
                targetGid = maxGid;
            } else {
                grp->lights.clear();
                for (const auto& lidVal : lightIds) {
                    AlienFX_SDK::Afx_groupLight gl;
                    gl.did = pid;
                    gl.lid = static_cast<WORD>(lidVal.asInt(0));
                    grp->lights.push_back(gl);
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

            targetSet->color.clear();
            AlienFX_SDK::Afx_action act1{};
            act1.type = EffectNameToType(eff);
            act1.r = r1; act1.g = g1; act1.b = b1;
            act1.tempo = speed;
            act1.time = 5;
            targetSet->color.push_back(act1);

            if (eff == "morph" || eff == "breathing" || eff == "pulse") {
                AlienFX_SDK::Afx_action act2{};
                act2.type = act1.type;
                act2.r = r2; act2.g = g2; act2.b = b2;
                act2.tempo = speed;
                act2.time = 5;
                targetSet->color.push_back(act2);
            }

            fxhl->Refresh(true);
            conf->Save();
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

    if (cmd == "bindLightMapping" && conf) {
        WORD pid = static_cast<WORD>(msg.get("pid").asInt(0));
        byte lid = static_cast<byte>(msg.get("lightId").asInt(0));
        std::string name = msg.get("name").asString();
        WORD scancode = static_cast<WORD>(msg.get("scancode").asInt(0));
        AlienFX_SDK::Afx_device* dev = conf->afx_dev.GetDeviceById(pid);
        if (dev) {
            for (auto& l : dev->lights) {
                if (l.lightid == lid) {
                    if (!name.empty()) l.name = name;
                    if (scancode) l.scancode = scancode;
                    break;
                }
            }
            conf->Save();
        }
        return BuildFullStateJson();
    }

    if (cmd == "activateProfile" && conf && eve) {
        unsigned id = static_cast<unsigned>(msg.get("id").asInt(0));
        profile* p = conf->FindProfile(id);
        if (p) {
            eve->SwitchActiveProfile(p, true);
        }
        return BuildFullStateJson();
    }

    if (cmd == "createProfile" && conf) {
        std::string name = msg.get("name").asString("New Profile");
        unsigned maxId = 1;
        for (const auto& p : conf->profiles) {
            if (p->id >= maxId) maxId = p->id + 1;
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
        byte mode = static_cast<byte>(msg.get("mode").asInt(0));
        mon->SetPowerMode(mode);
        fan_conf->Save();
        return BuildTelemetryJson();
    }

    if (cmd == "setFanCurve" && fan_conf && mon) {
        byte fanId = static_cast<byte>(msg.get("fanId").asInt(0));
        WORD sensorId = static_cast<WORD>(msg.get("sensorId").asInt(0));
        bool active = msg.get("active").asBool(true);
        const auto& pts = msg.get("points").arrVal;
        if (fan_conf->lastProf) {
            sen_block& blk = fan_conf->lastProf->fanControls[fanId][sensorId];
            blk.active = active;
            blk.points.clear();
            for (const auto& pt : pts) {
                fan_point fp;
                fp.temp = static_cast<byte>(pt.get("temp").asInt(40));
                fp.boost = static_cast<byte>(pt.get("boost").asInt(50));
                blk.points.push_back(fp);
            }
            fan_conf->Save();
        }
        return "{\"ok\":true}";
    }

    if (cmd == "setSettings" && conf) {
        if (msg.has("startWindows")) {
            bool en = msg.get("startWindows").asBool(false);
            bool min = msg.has("startMinimized") ? msg.get("startMinimized").asBool(true) : (conf->startMinimized != 0);
            conf->startWindows = en ? 1 : 0;
            StartupManager::SetStartupEnabled(en, min);
        }
        if (msg.has("startMinimized")) {
            conf->startMinimized = msg.get("startMinimized").asBool(false) ? 1 : 0;
            if (StartupManager::IsStartupEnabled())
                StartupManager::SetStartupEnabled(true, conf->startMinimized != 0);
        }
        if (msg.has("gammaCorrection")) conf->gammaCorrection = msg.get("gammaCorrection").asBool(true) ? 1 : 0;
        if (msg.has("enableProfSwitch")) conf->enableProfSwitch = msg.get("enableProfSwitch").asBool(true) ? 1 : 0;
        if (msg.has("enableEffects")) conf->enableEffects = msg.get("enableEffects").asBool(true) ? 1 : 0;
        if (msg.has("offOnBattery")) conf->offOnBattery = msg.get("offOnBattery").asBool(false) ? 1 : 0;
        if (msg.has("dimmedBatt")) conf->dimmedBatt = msg.get("dimmedBatt").asBool(false) ? 1 : 0;
        if (msg.has("fanControl")) {
            conf->fanControl = msg.get("fanControl").asBool(false) ? 1 : 0;
            if (eve) eve->ToggleFans();
        }
        conf->Save();
        if (fxhl) fxhl->Refresh(true);
        return BuildFullStateJson();
    }

    return "{\"ok\":false,\"error\":\"unknown_command\"}";
}
