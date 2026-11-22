#include "MonHelper.h"
#include <powrprof.h>

#pragma comment(lib, "PowrProf.lib")

// debug print
#ifdef _DEBUG
#define DebugPrint(_x_) OutputDebugString(string(_x_).c_str());
#else
#define DebugPrint(_x_)
#endif

void CMonProc(LPVOID);

extern ConfigFan* fan_conf;

//fan_profile* active = NULL;

MonHelper::MonHelper() {
	acpi = new AlienFan_SDK::Control();
	IIDFromString(L"{be337238-0d82-4146-a960-4f3749d470c7}", &perfset);
	if (acpi->Probe(fan_conf->diskSensors)) {
		fan_conf->lastSelectedSensor = acpi->sensors.empty() ? 0 : acpi->sensors.front().sid;
		fansize = (WORD)acpi->fans.size();
		powerSize = (WORD)acpi->powers.size();
		sensorSize = (WORD)acpi->sensors.size();
		oldPower = GetPowerMode();
		Start();
		GetSensorData(true);
	}
}

MonHelper::~MonHelper() {
	Stop();
	delete acpi;
}

void MonHelper::SetOC(bool force)
{
	if (!acpi || !acpi->isSupported) return;
	if (fan_conf->ocEnable || force) {
		if (acpi->isTcc && fan_conf->lastProf->currentTCC > 0) {
			acpi->SetTCC(fan_conf->lastProf->currentTCC);
		}
		if (acpi->isXMP) {
			acpi->SetXMP(fan_conf->lastProf->memoryXMP);
		}
	}
}

void MonHelper::ResetBoost() {
	lastBoost.clear();
	if (!acpi || !acpi->isSupported) return;
	if (!powerMode) {
		DebugPrint("Mon: Boost reset\n");
		for (int i = 0; i < fansize; i++) {
			acpi->SetFanBoost(i, 0);
		}
	}
}

void MonHelper::Run() {
	if (!acpi || !acpi->isSupported) return;
	if (!powerMode && !monThread) {
		monThread = new ThreadHelper(CMonProc, this, fan_conf->pollingRate, THREAD_PRIORITY_NORMAL/*THREAD_PRIORITY_BELOW_NORMAL*/);
		DebugPrint("Mon thread start.\n");
	}
}

void MonHelper::Finish() {
	if (monThread) {
		delete monThread;
		monThread = NULL;
		DebugPrint("Mon thread stop.\n");
		ResetBoost();
	}
}

void MonHelper::ToggleMode() {
	if (powerMode) {
		Finish();
		return;
	}
	if (!stopped)
		Run();
}

void MonHelper::Start() {
	if (!acpi || !acpi->isSupported) return;
	SetOC();
	SetCurrentMode();
	// start thread...
	Run();
	stopped = false;
}

void MonHelper::Stop() {
	Finish();
	if (acpi && acpi->isSupported && fan_conf->keepSystem)
		SetCurrentMode(oldPower);
	stopped = true;
}

void MonHelper::SetCurrentMode(int newMode) {
	if (!acpi || !acpi->isSupported || powerSize == 0) return;
	if (newMode < 0) {
		newMode = fan_conf->lastProf->gmodeStage ? powerSize :
			fan_conf->acPower ? fan_conf->lastProf->powerStage : fan_conf->lastProf->powerStageDC;
	}
	bool wasManualFullSpeed = fullSpeedManual;
	if (newMode != powerMode || wasManualFullSpeed) {
		if (newMode < powerSize) {
			fullSpeedManual = false;
			if (powerMode == powerSize || (acpi->isGmode && acpi->GetGMode() > 0)) {
				acpi->SetGMode(0);
				acpi->SetPower(0xa0);
			}
			ResetBoost();
			acpi->SetPower(acpi->powers[newMode]);
		}
		else {
			acpi->SetGMode(1);
			if (!acpi->isGmode || acpi->GetGMode() <= 0) {
				fullSpeedManual = true;
				acpi->SetPower(acpi->powers[0]);
				for (int i = 0; i < fansize; i++) {
					acpi->SetFanBoost(i, 100);
				}
				powerMode = 0;
				ToggleMode();
				return;
			}
		}
		powerMode = newMode;
		ToggleMode();
	}
}

void MonHelper::SetFullSpeed() {
	if (!acpi || !acpi->isSupported || powerSize == 0) return;
	fan_conf->lastProf->gmodeStage = true;
	if (acpi->isGmode) {
		fullSpeedManual = false;
		acpi->SetGMode(1);
		if (acpi->GetGMode() > 0) {
			powerMode = powerSize;
			ToggleMode();
			return;
		}
	}
	// Fallback when G-Mode WMI doesn't engage: switch to unlocked manual mode and lock all fans to 100% boost
	fullSpeedManual = true;
	if (fan_conf->acPower)
		fan_conf->lastProf->powerStage = 0;
	else
		fan_conf->lastProf->powerStageDC = 0;
	acpi->SetGMode(0);
	acpi->SetPower(acpi->powers[0]);
	powerMode = 0;
	for (int i = 0; i < fansize; i++) {
		acpi->SetFanBoost(i, 100);
	}
	if (!stopped) Run();
}

byte MonHelper::GetFanPercent(byte fanID)
{
	auto bst = &fan_conf->boosts[fanID];
	WORD rpm = fanRpm[fanID];
	int maxR = acpi ? acpi->GetMaxRPM(fanID) : -1;
	if (maxR > 1000 && bst->maxRPM < (WORD)maxR) {
		bst->maxRPM = (WORD)maxR;
	}
	if (!bst->maxRPM) { // no MaxRPM yet
		WORD fallbackMax = (maxR > 1000) ? (WORD)maxR : max((WORD)5200, rpm);
		*bst = { 100, fallbackMax };
	}
	if (bst->maxRPM < rpm) { // current RPM higher, then BIOS high
		bst->maxRPM = rpm;
	}
	if (rpm == 0) return 0;
	int acpiPct = acpi ? acpi->GetFanPercent(fanID) : -1;
	if (acpiPct > 0 && acpiPct <= 100) {
		return (byte)acpiPct;
	}
	return (byte)min(100, max(1, (int)((rpm * 100) / max((WORD)1000, bst->maxRPM))));
}

int MonHelper::GetPowerMode() {
	if (!acpi || !acpi->isSupported) return 0;
#ifdef _DEBUG
	int res = acpi->GetGMode() ? powerSize : acpi->GetPower();
	//DebugPrint("Mon: BIOS mode " + to_string(res) + ", current " + to_string(powerMode) + "\n");
	return res;
#else
	return acpi->GetGMode() ? powerSize : acpi->GetPower();
#endif // _DEBUG
}

void MonHelper::SetPowerMode(byte newMode) {
	if (!acpi || !acpi->isSupported || powerSize == 0) return;
	fullSpeedManual = false;
	if (newMode < powerSize) {
		if (fan_conf->acPower)
			fan_conf->lastProf->powerStage = newMode;
		else
			fan_conf->lastProf->powerStageDC = newMode;
	}
	fan_conf->lastProf->gmodeStage = newMode >= powerSize;
	SetCurrentMode(newMode);
}

void MonHelper::SetCpuModes() {
	if (PowerGetActiveScheme(NULL, &sch_guid) == ERROR_SUCCESS && sch_guid) {
		PowerWriteACValueIndex(NULL, sch_guid, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &perfset, fan_conf->lastProf->acMode);
		PowerWriteDCValueIndex(NULL, sch_guid, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &perfset, fan_conf->lastProf->dcMode);
		PowerSetActiveScheme(NULL, sch_guid);
		LocalFree(sch_guid);
		sch_guid = NULL;
	}
}

void MonHelper::SetCpuThrottleMax(DWORD acPct, DWORD dcPct) {
	acPct = max((DWORD)25, min((DWORD)100, acPct));
	dcPct = max((DWORD)25, min((DWORD)100, dcPct));
	fan_conf->cpuMaxAc = acPct;
	fan_conf->cpuMaxDc = dcPct;
	GUID* activeScheme = NULL;
	if (PowerGetActiveScheme(NULL, &activeScheme) == ERROR_SUCCESS && activeScheme) {
		PowerWriteACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MAXIMUM, acPct);
		PowerWriteDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MAXIMUM, dcPct);
		PowerSetActiveScheme(NULL, activeScheme);
		LocalFree(activeScheme);
	}
}

void MonHelper::ReadCpuSettings(DWORD& acBoost, DWORD& dcBoost, DWORD& acMaxPct, DWORD& dcMaxPct) {
	acBoost = fan_conf ? fan_conf->lastProf->acMode : 2;
	dcBoost = fan_conf ? fan_conf->lastProf->dcMode : 2;
	acMaxPct = fan_conf ? fan_conf->cpuMaxAc : 100;
	dcMaxPct = fan_conf ? fan_conf->cpuMaxDc : 100;
	GUID* activeScheme = NULL;
	if (PowerGetActiveScheme(NULL, &activeScheme) == ERROR_SUCCESS && activeScheme) {
		DWORD val = 0;
		if (PowerReadACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &perfset, &val) == ERROR_SUCCESS && val <= 6) {
			acBoost = val;
		}
		if (PowerReadDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &perfset, &val) == ERROR_SUCCESS && val <= 6) {
			dcBoost = val;
		}
		if (PowerReadACValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MAXIMUM, &val) == ERROR_SUCCESS && val <= 100) {
			acMaxPct = val;
		}
		if (PowerReadDCValueIndex(NULL, activeScheme, &GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MAXIMUM, &val) == ERROR_SUCCESS && val <= 100) {
			dcMaxPct = val;
		}
		LocalFree(activeScheme);
	}
}

int MonHelper::FindPowerIndexByCodes(const vector<byte>& preferredCodes) const {
	if (!acpi || !acpi->isSupported || acpi->powers.empty()) return -1;
	for (byte code : preferredCodes) {
		for (int i = 0; i < (int)acpi->powers.size(); ++i) {
			if (acpi->powers[i] == code) return i;
		}
	}
	return -1;
}

byte MonHelper::GetCurrentBiosCode() const {
	if (!acpi || !acpi->isSupported) return 0;
	if (fullSpeedManual || (powerSize > 0 && powerMode >= powerSize)) return 0xAB;
	if (powerMode < acpi->powers.size()) return acpi->powers[powerMode];
	int raw = acpi->GetPower(true);
	return raw >= 0 ? (byte)raw : 0;
}

string MonHelper::GetBiosPowerModeName(byte code) {
	switch (code) {
	case 0x00: return "Custom / Manual";
	case 0x96: return "Quiet";
	case 0x97: return "Balanced";
	case 0x98: return "Balanced Performance";
	case 0x99: return "Performance";
	case 0xA0: return "Balanced";
	case 0xA1: return "Balanced Performance";
	case 0xA2: return "Cool";
	case 0xA3: return "Quiet";
	case 0xA4: return "Performance";
	case 0xA5: return "Battery Saver";
	case 0xAB: return "Overdrive (G-Mode)";
	default: {
		char buf[32]{};
		snprintf(buf, sizeof(buf), "BIOS Mode 0x%02X", code);
		return buf;
	}
	}
}

// I need this wrapper for buggy G-series BIOS which return error from time to time
int MonHelper::GetFanRPM(int fanID) {
	int res;
	return (res = acpi->GetFanRPM(fanID)) < 0 ? fanRpm[fanID] : res;
}

WORD MonHelper::GetSensorData(bool forced) {
	if (!acpi || !acpi->isSupported) return 0;
	std::lock_guard<std::mutex> lock(dataMutex);
	// update values:
	if (forced || !monThread) {
		// temps..
#ifndef FANV1
		if (acpi->m_Refresher) acpi->m_Refresher->Refresh(0);
#endif
		modified = false;
		for (int i = 0; i < sensorSize; i++) {
			int temp = acpi->GetTempValue(i);
			WORD sid = acpi->sensors[i].sid;
			if (temp != senValues[sid]) {
				modified = true;
				senValues[sid] = temp;
				maxTemps[sid] = max(maxTemps[sid], temp);
			}
		}
		// fans...
		for (byte i = 0; i < fansize; i++) {
			fanRpm[i] = GetFanRPM(i);
		}
	}
	return GetPowerMode();
}

void MonHelper::GetTelemetrySnapshot(map<WORD, short>& outSen, map<WORD, short>& outMax, vector<pair<byte, pair<WORD, byte>>>& outFans) {
	std::lock_guard<std::mutex> lock(dataMutex);
	outSen = senValues;
	outMax = maxTemps;
	outFans.clear();
	outFans.reserve(fanRpm.size());
	for (const auto& kv : fanRpm) {
		outFans.push_back({ kv.first, { kv.second, GetFanPercent(kv.first) } });
	}
}

void CMonProc(LPVOID param) {
	MonHelper* src = (MonHelper*)param;
	AlienFan_SDK::Control* acpi = src->acpi;
	fan_profile* active = fan_conf->lastProf;

	if (src->inControl && active) {
		// check power mode
		if (src->fullSpeedManual) {
			src->GetSensorData(true);
			for (int i = 0; i < src->fansize; i++) {
				if (acpi->GetFanBoost(i) < 100) {
					acpi->SetFanBoost(i, 100);
				}
			}
			return;
		}
		src->powerMode = src->GetSensorData(true);
		src->SetCurrentMode();

		if (!src->powerMode) {
			int cBoost;
			for (auto& cIter : active->fanControls) {
				// Check boost
				byte fanID = cIter.first;
				int curBoost = 0;
				{
					std::lock_guard<std::mutex> lock(src->dataMutex);
					for (auto fIter = cIter.second.begin(); fIter != cIter.second.end(); fIter++) {
						if (fIter->second.active) {
							sen_block* cur = &fIter->second;
							WORD senID = fIter->first;
							if (cur->points.size() < 2) continue;
							auto k = cur->points.begin() + 1;
							for (; k != cur->points.end() && src->senValues[senID] > k->temp; k++);
							auto ko = k - 1;
							cBoost = k == cur->points.end() || k->temp == ko->temp ? ko->boost :
								ko->boost + ((k->boost - ko->boost) * (src->senValues[senID] - ko->temp))
								/ (k->temp - ko->temp);
							src->senBoosts[fanID][senID] = cBoost;
							if (cBoost > curBoost) {
								curBoost = cBoost;
								src->lastBoost[fanID] = senID;
							}
						}
					}
				}
				// Set boost
				int curBoostRaw = (int)round((fan_conf->GetFanScale(fanID) * curBoost) / 100.0);
				if (curBoostRaw < 101 || !src->fanSleep[fanID]) {
					byte boostOld = src->acpi->GetFanBoost(fanID);
					// Check overboost tricks...
					if (boostOld < 90 && curBoostRaw > 100) {
						curBoostRaw = 100;
						src->fanSleep[fanID] = ((100 - boostOld) >> 3) + 2;
						DebugPrint("Overboost started, fan " + to_string(fanID) + " locked for " + to_string(src->fanSleep[fanID]) + " tacts (old "
							+ to_string(boostOld) + ", new " + to_string(curBoostRaw) + ")!\n");
					}
					else
						src->fanSleep[fanID] = 0;
					if (curBoostRaw != boostOld) {
						acpi->SetFanBoost(fanID, curBoostRaw);
					}
				}
				else
					--src->fanSleep[fanID];
			}
		}
	}
}