#define WIN32_LEAN_AND_MEAN

#include "alienfan-SDK.h"
#include "alienfan-controls.h"

#pragma comment(lib, "wbemuuid.lib")

//#define _TRACE_

namespace AlienFan_SDK {

	Control::Control() {

#ifdef _TRACE_
		printf("WMI activation started.\n");
#endif
		VariantInit(&m_instancePath);
		IWbemLocator* m_WbemLocator = NULL;
		HRESULT hrCom = CoInitializeEx(nullptr, COINIT::COINIT_MULTITHREADED);
		comNeedUninit = (hrCom == S_OK || hrCom == S_FALSE);
		CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
			RPC_C_AUTHN_LEVEL_NONE, //RPC_C_AUTHN_LEVEL_CONNECT,
			RPC_C_IMP_LEVEL_IMPERSONATE,
			nullptr, EOAC_NONE, nullptr);

		if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&m_WbemLocator)) && m_WbemLocator) {
			m_WbemLocator->ConnectServer((BSTR)L"ROOT\\WMI", nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &m_WbemServices);
			m_WbemLocator->ConnectServer((BSTR)L"ROOT\\Microsoft\\Windows\\Storage\\Providers_v2", nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &m_DiskService);
			m_WbemLocator->ConnectServer((BSTR)L"ROOT\\LibreHardwareMonitor", nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &m_OHMService);
			m_WbemLocator->Release();
		}
		if (SUCCEEDED(CoCreateInstance(CLSID_WbemRefresher, NULL, CLSCTX_INPROC_SERVER, IID_IWbemRefresher, (void**)&m_Refresher)) && m_Refresher) {
			m_Refresher->QueryInterface(IID_IWbemConfigureRefresher, (void**)&m_pConfig);
		}
	}

	Control::~Control() {
		VariantClear(&m_instancePath);
		if (m_pConfig) {
			m_pConfig->Release();
			m_pConfig = NULL;
		}
		if (m_Refresher) {
			m_Refresher->Release();
			m_Refresher = NULL;
		}
		if (m_AWCCGetObj)
			m_AWCCGetObj->Release();
		if (m_DiskService)
			m_DiskService->Release();
		if (m_OHMService)
			m_OHMService->Release();
		if (m_InParamaters)
			m_InParamaters->Release();
		if (m_WbemServices)
			m_WbemServices->Release();
		if (comNeedUninit)
			CoUninitialize();
	}

	int Control::CallWMIMethod(byte com, byte arg1, byte arg2) {
		std::lock_guard<std::mutex> lock(wmiMutex);
		VARIANT result{ VT_I4 };
		result.intVal = -1;
		if (sysType >= 0 && m_WbemServices && m_InParamaters && m_instancePath.vt == VT_BSTR && m_instancePath.bstrVal) {
			IWbemClassObject* m_outParameters = NULL;
			VARIANT parameters = { VT_I4 };
			parameters.uintVal = ALIENFAN_INTERFACE{ dev_controls[com], arg1, arg2, 0 }.args;
			m_InParamaters->Put(L"arg2", NULL, &parameters, 0);
			if (SUCCEEDED(m_WbemServices->ExecMethod(m_instancePath.bstrVal,
				commandList[functionID[sysType][com]], 0, NULL, m_InParamaters, &m_outParameters, NULL)) && m_outParameters) {
				m_outParameters->Get(L"argr", 0, &result, nullptr, nullptr);
				m_outParameters->Release();
			}
		}
		return result.intVal;
	}

	void Control::EnumSensors(IWbemServices* srv, const wchar_t* s_name, const LPCWSTR valuePath, string name, byte type) {
		if (!srv || !m_pConfig || !m_Refresher) return;
		long plID = 0;
		IWbemHiPerfEnum* insts = NULL;
		m_pConfig->AddEnum(srv, (BSTR)s_name, 0, NULL, &insts, &plID);
		if (insts) {
			vector<IWbemObjectAccess*> spInstance;
			ULONG uNumOfInstances = 0;
			m_Refresher->Refresh(0);
			insts->GetObjects(0, 0, nullptr, &uNumOfInstances);
			if (uNumOfInstances) {
				spInstance.resize(uNumOfInstances, nullptr);
				if (SUCCEEDED(insts->GetObjects(0, uNumOfInstances, spInstance.data(), &uNumOfInstances)) && uNumOfInstances) {
					byte senID = 0;
					for (ULONG idx = 0; idx < uNumOfInstances; ++idx) {
						auto& i = spInstance[idx];
						if (!i) continue;
						VARIANT cTemp{}, vtype{}, vname{};
						VariantInit(&cTemp);
						VariantInit(&vtype);
						VariantInit(&vname);
						string lname;
						if (type == 4) { // OHM sensors
							if (SUCCEEDED(i->Get(L"SensorType", 0, &vtype, 0, 0)) && vtype.vt == VT_BSTR && vtype.bstrVal &&
								!wcscmp(vtype.bstrVal, L"Temperature")) {
								if (SUCCEEDED(i->Get(L"Name", 0, &vname, 0, 0)) && vname.vt == VT_BSTR && vname.bstrVal) {
									for (size_t k = 0; k < wcslen(vname.bstrVal); k++)
										lname += (char)vname.bstrVal[k];
								}
							}
							else {
								VariantClear(&cTemp);
								VariantClear(&vtype);
								VariantClear(&vname);
								i->Release();
								continue;
							}
						}
						else {
							lname = name + " sensor " + to_string(senID);
						}
						if (SUCCEEDED(i->Get(valuePath, 0, &cTemp, 0, 0)) &&
							(type == 2 || (cTemp.vt == VT_I4 && cTemp.intVal > 0) || (cTemp.vt == VT_R4 && cTemp.fltVal > 0))) {
							sensors.push_back({ { senID++,type }, lname, i, (BSTR)valuePath });
						}
						else {
							i->Release();
						}
						VariantClear(&cTemp);
						VariantClear(&vtype);
						VariantClear(&vname);
					}
				}
			}
			insts->Release();
#ifdef _TRACE_
			printf("%d sensors of #%d added, %d total\n", uNumOfInstances, type, (int)sensors.size());
#endif
		}
	}

	bool Control::Probe(bool diskSensors) {
		IEnumWbemClassObject* enum_obj = NULL;
#ifdef _TRACE_
		if (m_WbemServices)
			printf("WMI connect succesful!\n");
#endif
		if (m_WbemServices && (isAlienware = SUCCEEDED(m_WbemServices->GetObject((BSTR)L"AWCCWmiMethodFunction", NULL, nullptr, &m_AWCCGetObj, nullptr))) && m_AWCCGetObj) {
#ifdef _TRACE_
			printf("AWCC section detected!\n");
#endif
			// need to get instance
			if (SUCCEEDED(m_WbemServices->CreateInstanceEnum((BSTR)L"AWCCWmiMethodFunction", WBEM_FLAG_FORWARD_ONLY, NULL, &enum_obj)) && enum_obj) {

				IWbemClassObject* spInstance = NULL;
				ULONG uNumOfInstances = 0;
				HRESULT hrNext = enum_obj->Next(10000, 1, &spInstance, &uNumOfInstances);
				if (hrNext == S_OK && uNumOfInstances > 0 && spInstance) {
					spInstance->Get((BSTR)L"__Path", 0, &m_instancePath, 0, 0);
					spInstance->Release();
					enum_obj->Release();
					enum_obj = NULL;

					if (m_instancePath.vt != VT_BSTR || !m_instancePath.bstrVal) {
						return false;
					}

					// check system type and fill inParams
					for (sysType = 0; sysType < 2; sysType++)
						if (isSupported = (SUCCEEDED(m_AWCCGetObj->GetMethod(commandList[functionID[sysType][getPowerID]], NULL, &m_InParamaters, nullptr)) && m_InParamaters)) {
#ifdef _TRACE_
							printf("Fan Control available, system type %d\n", sysType);
#endif
								systemID = CallWMIMethod(getSysID, 2);
#ifdef _TRACE_
								printf("System ID = %d\n", systemID);
#endif
							isGmode = SUCCEEDED(m_AWCCGetObj->GetMethod(commandList[2], NULL, nullptr, nullptr));
#ifdef _TRACE_
							if (isGmode)
								printf("G-Mode available\n");

#endif
							if (isTcc = SUCCEEDED(m_AWCCGetObj->GetMethod(commandList[6], NULL, nullptr, nullptr))) {
								int mt = CallWMIMethod(getMaxTCC);
								int mo = CallWMIMethod(getMaxOffset);
								if (mt > 50 && mt <= 120 && mo > 0 && mo <= 50) {
									maxTCC = (byte)mt;
									maxOffset = (byte)mo;
								} else {
									isTcc = false;
									maxTCC = 100;
									maxOffset = 0;
								}
							}
#ifdef _TRACE_
							if (isTcc)
								printf("TCC control available\n");
#endif
							isXMP = SUCCEEDED(m_AWCCGetObj->GetMethod(commandList[7], NULL, nullptr, nullptr));
#ifdef _TRACE_
							if (isXMP)
								printf("Memory XMP available\n");
#endif
							int fIndex = 0, funcID;

							powers.push_back(0); // Manual mode
							// Scan for avaliable data
							while ((funcID = CallWMIMethod(getPowerID, fIndex)) > 0) {
								byte vkind = funcID & 0xff;
								if (funcID > 0x100 && funcID < 0x110) {
									// sensor
#ifdef _TRACE_
									printf("Sensor ID=%x found\n", funcID);
#endif
									sensors.push_back({ { vkind, 1 }, sensors.size() < 3 ? temp_names[sensors.size()] : "Sensor #" + to_string(sensors.size()) });
								}
								else {
									if (funcID > 0x8f) {
										// power mode
										powers.push_back(vkind);
#ifdef _TRACE_
										printf("Power ID=%x found\n", funcID);
#endif
									}
									else {
										// fan
										fans.push_back({ vkind, (byte)CallWMIMethod(getFanSensor, vkind) });
#ifdef _TRACE_
										printf("Fan ID=%x found\n", funcID);
#endif
									}
								}
								fIndex++;
							}
#ifdef _TRACE_
							printf("%d fans, %d sensors, %d Power modes found, last reply %x\n", (int)fans.size(), (int)sensors.size(), (int)powers.size(), funcID);
#endif
							if (sysType) {
								// Modes 1 and 2 for R7 desktop
								powers.push_back(1);
								powers.push_back(2);
							}

							// ESIF sensors
							EnumSensors(m_WbemServices, L"EsifDeviceInformation", L"Temperature", "ESIF", 0);
							// SSD sensors
							if (diskSensors)
								EnumSensors(m_DiskService, L"MSFT_PhysicalDiskToStorageReliabilityCounter", L"Temperature", "SSD", 2);
							// OHM sensors
							EnumSensors(m_OHMService, L"Sensor", L"Value", "", 4);
							return true;
						}
				}
				if (enum_obj) enum_obj->Release();
			}
		}
#ifdef _TRACE_
		else
			printf("AWCC section NOT detected!\n");
#endif
		return false;
	}

	inline int Control::OperateFan(int function, int fanID) {
		return fanID < fans.size() ? CallWMIMethod(function, (byte)fans[fanID].id) : -1;
	}

	int Control::GetFanRPM(int fanID) {
		return OperateFan(getFanRPM, fanID);
	}
	int Control::GetMaxRPM(int fanID)
	{
		return OperateFan(getMaxRPM, fanID);
	}
	int Control::GetFanPercent(int fanID) {
		return OperateFan(getFanPercent, fanID);
	}
	int Control::GetFanBoost(int fanID) {
		return OperateFan(getFanBoost, fanID);
	}
	int Control::SetFanBoost(int fanID, byte value) {
		return fanID < fans.size() ? CallWMIMethod(setFanBoost, (byte)fans[fanID].id, value) : -1;
	}
	int Control::GetTempValue(int TempID) {
		if (TempID >= 0 && TempID < (int)sensors.size()) {
			if (sensors[TempID].type == 1) {
				// AWCC
				int awt = CallWMIMethod(getTemp, sensors[TempID].index);
				// Bugfix for AWCC temp - it can be up to 5000C!
				return awt > 150 ? -1 : awt;
			}
			else if (sensors[TempID].instance) {
				// ESIF, SSD, OHM
				VARIANT temp{};
				VariantInit(&temp);
				int val = -1;
				if (SUCCEEDED(sensors[TempID].instance->Get(sensors[TempID].valueName, 0, &temp, 0, 0))) {
					val = sensors[TempID].type == 4 ? (int)temp.fltVal : temp.intVal;
				}
				VariantClear(&temp);
				return val;
			}
		}
		return -1;
	}
	int Control::Unlock() {
		return SetPower(0);
	}
	int Control::SetPower(byte level) {
		return CallWMIMethod(setPowerMode, level);
	}
	int Control::GetPower(bool raw) {
		int pl = CallWMIMethod(getPowerMode);
		if (raw || pl < 0) return pl;
		for (int i = 0; i < powers.size(); i++)
			if (powers[i] == pl)
				return i;
		return -1;
	}

	int Control::SetGMode(bool state)
	{
		if (isGmode) {
			if (state)
				SetPower(0xab);
			return CallWMIMethod(setGMode, state);
		}
		return -1;
	}

	int Control::GetGMode() {
		if (isGmode) {
			int pm = GetPower(true);
			if (pm == 0xab) return 1;
			return CallWMIMethod(getGMode) == 1 ? 1 : 0;
		}
		return 0;
	}

	int Control::GetTCC()
	{
		if (isTcc) {
			int curOffset = CallWMIMethod(getCurrentOffset);
			if (curOffset >= 0 && curOffset <= maxOffset) {
				return maxTCC - curOffset;
			}
		}
		return -1;
	}

	int Control::SetTCC(byte tccValue)
	{
		if (isTcc && tccValue <= maxTCC && (maxTCC - tccValue) <= maxOffset) {
			return CallWMIMethod(setOffset, maxTCC - tccValue);
		}
		return -1;
	}

	int Control::GetXMP()
	{	if (isXMP)
			return CallWMIMethod(getXMP);
		return -1;
	}

	int Control::SetXMP(byte memXMP)
	{
		if (isXMP) {
			int res = CallWMIMethod(setXMP, memXMP);
			return res ? -1 : res;
		}
		return -1;
	}

	Lights::Lights(Control* ac) {
		VariantInit(&m_instancePath);
		if (ac && ac->isAlienware && ac->m_WbemServices && ac->m_AWCCGetObj &&
			ac->m_instancePath.vt == VT_BSTR && ac->m_instancePath.bstrVal &&
			SUCCEEDED(ac->m_AWCCGetObj->GetMethod(colorList[0], NULL, &m_InParamaters[0], nullptr)) && m_InParamaters[0]) {
			ac->m_AWCCGetObj->GetMethod(colorList[1], NULL, &m_InParamaters[1], nullptr);
			m_WbemServices = ac->m_WbemServices;
			m_WbemServices->AddRef();
			VariantCopy(&m_instancePath, &ac->m_instancePath);
			isActivated = true;
		}
	}

	Lights::~Lights() {
		if (m_InParamaters[0]) {
			m_InParamaters[0]->Release();
			m_InParamaters[0] = NULL;
		}
		if (m_InParamaters[1]) {
			m_InParamaters[1]->Release();
			m_InParamaters[1] = NULL;
		}
		if (m_WbemServices) {
			m_WbemServices->Release();
			m_WbemServices = NULL;
		}
		VariantClear(&m_instancePath);
	}

	int Lights::CallWMIMethod(byte com, byte* arg1) {
		int retVal = -1;
		if (com < 2 && m_InParamaters[com] && m_WbemServices &&
			m_instancePath.vt == VT_BSTR && m_instancePath.bstrVal) {
			SAFEARRAY* args = SafeArrayCreateVector(VT_UI1, 0, 8);
			if (!args) return -1;
			for (long i = 0; i < 8; i++) {
				SafeArrayPutElement(args, &i, &arg1[i]);
			}
			VARIANT parameters{};
			VariantInit(&parameters);
			parameters.vt = VT_ARRAY | VT_UI1;
			parameters.parray = args;
			m_InParamaters[com]->Put(L"arg2", 0, &parameters, 0);

			IWbemClassObject* m_outParameters = NULL;
			if (SUCCEEDED(m_WbemServices->ExecMethod(m_instancePath.bstrVal,
				colorList[com], 0, NULL, m_InParamaters[com], &m_outParameters, NULL)) && m_outParameters) {
				VARIANT result{};
				VariantInit(&result);
				if (SUCCEEDED(m_outParameters->Get(L"argr", 0, &result, nullptr, nullptr))) {
					if (result.vt == VT_I4 || result.vt == VT_UI4) {
						retVal = result.intVal;
					}
				}
				VariantClear(&result);
				m_outParameters->Release();
			}
			VariantClear(&parameters);
		}
		return retVal;
	}

	bool Lights::SetBrightness(byte brightness) {
		byte param[8]{ 0 };
		param[4] = brightness;
		return CallWMIMethod(1, param) >= 0;
	}

	bool Lights::SetColor(DWORD mask, byte r, byte g, byte b, bool save) {
		byte param[8]{
			(byte)(mask & 0xff),
			(byte)((mask >> 8) & 0xff),
			(byte)((mask >> 16) & 0xff),
			(byte)((mask >> 24) & 0xff),
			b, g, r,
			(byte)(save ? 0 : 0xff)
		};
		return CallWMIMethod(0, param) >= 0;
	}

}
