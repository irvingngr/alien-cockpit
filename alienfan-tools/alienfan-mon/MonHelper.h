#pragma once
#include <mutex>
#include <utility>
#include "alienfan-SDK.h"
#include "ConfigFan.h"
#include "ThreadHelper.h"

class MonHelper {
private:
	ThreadHelper* monThread = NULL;
	short oldPower = 0;
	bool stopped = true;
	void Run();
	void Finish();
	void ToggleMode();
	GUID* sch_guid, perfset;
public:
	mutable std::mutex dataMutex;
	AlienFan_SDK::Control* acpi;
	bool inControl = true;
	map<byte,WORD> lastBoost, fanRpm;
	map<byte,byte> fanSleep;
	map<WORD, short> senValues, maxTemps;
	map<byte,map<WORD, byte>> senBoosts;
	WORD powerMode = 0;
	WORD fansize = 0, powerSize = 0, sensorSize = 0;
	bool modified = true;
	bool fullSpeedManual = false;

	MonHelper();
	~MonHelper();
	void Start();
	void Stop();
	void SetCurrentMode(int newMode = -1);
	void SetFullSpeed();
	byte GetFanPercent(byte fanID);
	int GetPowerMode();
	void SetPowerMode(byte newMode);
	int GetFanRPM(int fanID);
	WORD GetSensorData(bool forced = false);
	void GetTelemetrySnapshot(map<WORD, short>& outSen, map<WORD, short>& outMax, vector<pair<byte, pair<WORD, byte>>>& outFans);
	void ResetBoost();
	void SetOC();
	void SetCpuModes();
};

