#define _CRT_SECURE_NO_WARNINGS 
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup") 
#pragma comment(lib, "winmm.lib") 

#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h> 

#include "resource.h" 
#include <thread>
#include <atomic>
#include <sysinfoapi.h>
#include <processthreadsapi.h>
#include <random> 

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define ASIO_STANDALONE
#define CROW_MAIN
#include "crow.h"

#define WEBVIEW_WINAPI
#include "webview.h"

enum AppMode {
	MODE_WINDOW = 0,
	MODE_OVERLAY = 1
};

std::atomic<AppMode> g_currentMode{ MODE_WINDOW };
std::atomic<bool> g_isMuted{ false };
std::atomic<int> g_bgmVolume{ 100 };
std::atomic<bool> g_stopBgmThread{ false };

HWND g_hwndWebview = NULL;
WNDPROC g_oldWndProc = NULL;

std::string GetResourceData(int resourceId, const char* resourceType) {
	HMODULE hModule = GetModuleHandle(NULL);
	HRSRC hRes = FindResourceA(hModule, MAKEINTRESOURCEA(resourceId), resourceType);
	if (!hRes) return "";
	HGLOBAL hData = LoadResource(hModule, hRes);
	DWORD size = SizeofResource(hModule, hRes);
	char* data = (char*)LockResource(hData);
	if (!data || size == 0) return "";
	return std::string(data, size);
}

void SetBGMVolume(int volPercent) {
	if (volPercent < 0) volPercent = 0;
	if (volPercent > 100) volPercent = 100;
	g_bgmVolume = volPercent;

	int actualVol = volPercent;
	if (g_isMuted.load()) {
		actualVol = 0;
	}

	DWORD waveVol = (DWORD)((actualVol / 100.0f) * 0xFFFF);
	DWORD fullVol = (waveVol & 0xFFFF) | ((waveVol & 0xFFFF) << 16);
	waveOutSetVolume(NULL, fullVol);
}

void BGMThreadFunc() {
	std::vector<int> tracks = { IDR_WAVE1, IDR_WAVE2, IDR_WAVE3, IDR_WAVE4 };
	std::random_device rd;
	std::mt19937 gen(rd());

	while (!g_stopBgmThread) {
		std::uniform_int_distribution<> dis(0, tracks.size() - 1);
		int trackId = tracks[dis(gen)];
		PlaySoundA(MAKEINTRESOURCEA(trackId), GetModuleHandle(NULL), SND_RESOURCE | SND_SYNC);

		if (g_stopBgmThread) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void PlayBGM() {
	g_isMuted = false;
	SetBGMVolume(g_bgmVolume.load());
}

void StopBGM() {
	g_isMuted = true;
	SetBGMVolume(g_bgmVolume.load());
}

void ResumeBGM() {
	g_isMuted = false;
	SetBGMVolume(g_bgmVolume.load());
}

LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (g_currentMode == MODE_OVERLAY) {
		if (uMsg == WM_SHOWWINDOW && wParam == FALSE) return 0;
		if (uMsg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_MINIMIZE) return 0;
		if (uMsg == WM_WINDOWPOSCHANGING) {
			WINDOWPOS* pos = (WINDOWPOS*)lParam;
			if (pos->flags & SWP_HIDEWINDOW) {
				pos->flags &= ~SWP_HIDEWINDOW;
				pos->flags |= SWP_SHOWWINDOW;
			}
			pos->hwndInsertAfter = HWND_TOPMOST;
		}
	}
	return CallWindowProc(g_oldWndProc, hwnd, uMsg, wParam, lParam);
}

void SetAppMode(AppMode mode) {
	if (!g_hwndWebview) return;
	g_currentMode = mode;

	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);

	if (mode == MODE_WINDOW) {
		SetWindowLong(g_hwndWebview, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		SetWindowLong(g_hwndWebview, GWL_EXSTYLE, WS_EX_APPWINDOW);

		int w = 1600, h = 900;
		int x = (screenWidth - w) / 2;
		int y = (screenHeight - h) / 2;
		SetWindowPos(g_hwndWebview, HWND_NOTOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
	}
	else if (mode == MODE_OVERLAY) {
		int width = 210, height = 75, x = 20, y = 20;
		SetWindowLong(g_hwndWebview, GWL_STYLE, WS_POPUP | WS_VISIBLE);
		SetWindowLong(g_hwndWebview, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE);
		SetWindowPos(g_hwndWebview, HWND_TOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
}

void* __stdcall ADL_Main_Memory_Alloc(int iSize) { return malloc(iSize); }

struct ADLTemperature { int iSize; int iTemperature; };
struct ADLPMActivity {
	int iSize, iEngineClock, iMemoryClock, iVddc, iActivityPercent;
	int iCurrentPerformanceLevel, iCurrentBusSpeed, iCurrentBusLanes, iMaximumBusLanes, iReserved;
};
struct AdapterInfo {
	int iSize, iAdapterIndex;
	char strUDID[256];
	int iBusNumber, iDeviceNumber, iFunctionNumber, iVendorID;
	char strAdapterName[256], strDisplayName[256];
	int iPresent, iExist;
	char strDriverPath[256], strDriverPathExt[256], strPNPString[256];
	int iOSDisplayIndex;
};

typedef void* (__stdcall* ADL_MAIN_MALLOC_CALLBACK)(int);
typedef int (*ADL_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int (*ADL_MAIN_CONTROL_DESTROY)();
typedef int (*ADL_ADAPTER_NUMBEROFADAPTERS_GET)(int*);
typedef int (*ADL_ADAPTER_ADAPTERINFO_GET)(AdapterInfo*, int);
typedef int (*ADL_OVERDRIVE5_TEMPERATURE_GET)(int, int, ADLTemperature*);
typedef int (*ADL_OVERDRIVE5_CURRENTACTIVITY_GET)(int, ADLPMActivity*);

bool GetAmdGpuStats(double& usageOut, double& tempOut) {
	HMODULE hAdl = LoadLibraryA("atiadlxx.dll");
	if (!hAdl) hAdl = LoadLibraryA("atiadlxy.dll");
	if (!hAdl) return false;

	auto ADL_Main_Control_Create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(hAdl, "ADL_Main_Control_Create");
	auto ADL_Main_Control_Destroy = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(hAdl, "ADL_Main_Control_Destroy");
	auto ADL_Adapter_NumberOfAdapters_Get = (ADL_ADAPTER_NUMBEROFADAPTERS_GET)GetProcAddress(hAdl, "ADL_Adapter_NumberOfAdapters_Get");
	auto ADL_Adapter_AdapterInfo_Get = (ADL_ADAPTER_ADAPTERINFO_GET)GetProcAddress(hAdl, "ADL_Adapter_AdapterInfo_Get");
	auto ADL_Overdrive5_Temperature_Get = (ADL_OVERDRIVE5_TEMPERATURE_GET)GetProcAddress(hAdl, "ADL_Overdrive5_Temperature_Get");
	auto ADL_Overdrive5_CurrentActivity_Get = (ADL_OVERDRIVE5_CURRENTACTIVITY_GET)GetProcAddress(hAdl, "ADL_Overdrive5_CurrentActivity_Get");

	bool success = false;
	if (ADL_Main_Control_Create && ADL_Main_Control_Create(ADL_Main_Memory_Alloc, 1) == 0) {
		int numAdapters = 0;
		if (ADL_Adapter_NumberOfAdapters_Get && ADL_Adapter_NumberOfAdapters_Get(&numAdapters) == 0 && numAdapters > 0) {
			std::vector<AdapterInfo> adapterInfos(numAdapters);
			adapterInfos[0].iSize = sizeof(AdapterInfo);
			if (ADL_Adapter_AdapterInfo_Get && ADL_Adapter_AdapterInfo_Get(adapterInfos.data(), sizeof(AdapterInfo) * numAdapters) == 0) {
				for (int i = 0; i < numAdapters; i++) {
					if (adapterInfos[i].iExist && adapterInfos[i].iVendorID == 1002) {
						int adapterIndex = adapterInfos[i].iAdapterIndex;
						ADLTemperature adlTemp = { sizeof(ADLTemperature), 0 };
						if (ADL_Overdrive5_Temperature_Get && ADL_Overdrive5_Temperature_Get(adapterIndex, 0, &adlTemp) == 0) {
							tempOut = (double)adlTemp.iTemperature / 1000.0;
						}
						ADLPMActivity adlActivity = { sizeof(ADLPMActivity), 0 };
						if (ADL_Overdrive5_CurrentActivity_Get && ADL_Overdrive5_CurrentActivity_Get(adapterIndex, &adlActivity) == 0) {
							usageOut = (double)adlActivity.iActivityPercent;
						}
						success = true;
						break;
					}
				}
			}
		}
		if (ADL_Main_Control_Destroy) ADL_Main_Control_Destroy();
	}
	FreeLibrary(hAdl);
	return success;
}

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
typedef int (*nvmlInit_t)();
typedef int (*nvmlShutdown_t)();
typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int index, void** device);
typedef int (*nvmlDeviceGetTemperature_t)(void* device, int sensorType, unsigned int* temp);
typedef int (*nvmlDeviceGetUtilizationRates_t)(void* device, nvmlUtilization_t* rates);

void GetGpuStats(double& usageOut, double& tempOut) {
	usageOut = 0.0; tempOut = 0.0;
	HMODULE hNvml = LoadLibraryA("nvml.dll");
	if (hNvml) {
		auto nvmlInit = (nvmlInit_t)GetProcAddress(hNvml, "nvmlInit_v2");
		if (!nvmlInit) nvmlInit = (nvmlInit_t)GetProcAddress(hNvml, "nvmlInit");
		auto nvmlShutdown = (nvmlShutdown_t)GetProcAddress(hNvml, "nvmlShutdown");
		auto nvmlGetHandle = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex_v2");
		if (!nvmlGetHandle) nvmlGetHandle = (nvmlDeviceGetHandleByIndex_t)GetProcAddress(hNvml, "nvmlDeviceGetHandleByIndex");
		auto nvmlGetTemp = (nvmlDeviceGetTemperature_t)GetProcAddress(hNvml, "nvmlDeviceGetTemperature");
		auto nvmlGetUtil = (nvmlDeviceGetUtilizationRates_t)GetProcAddress(hNvml, "nvmlDeviceGetUtilizationRates");

		if (nvmlInit && nvmlInit() == 0) {
			void* device = nullptr;
			if (nvmlGetHandle && nvmlGetHandle(0, &device) == 0 && device) {
				unsigned int temp = 0;
				if (nvmlGetTemp && nvmlGetTemp(device, 0, &temp) == 0) tempOut = (double)temp;
				nvmlUtilization_t util = { 0 };
				if (nvmlGetUtil && nvmlGetUtil(device, &util) == 0) usageOut = (double)util.gpu;
				if (nvmlShutdown) nvmlShutdown();
				FreeLibrary(hNvml);
				return;
			}
			if (nvmlShutdown) nvmlShutdown();
		}
		FreeLibrary(hNvml);
	}
	GetAmdGpuStats(usageOut, tempOut);
}

std::string GetRealCpuName() {
	HKEY hKey; char cpuName[256] = "Unknown CPU"; DWORD bufSize = sizeof(cpuName);
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &bufSize);
		RegCloseKey(hKey);
	}
	return std::string(cpuName);
}

std::string GetRealTotalRam() {
	MEMORYSTATUSEX memInfo; memInfo.dwLength = sizeof(MEMORYSTATUSEX); GlobalMemoryStatusEx(&memInfo);
	double totalGB = (double)memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
	char buf[32]; snprintf(buf, sizeof(buf), "%.1f GB", totalGB);
	return std::string(buf);
}

std::vector<std::string> GetRealGpuName() {
	std::vector<std::string> gpuList;
	DISPLAY_DEVICEA dd; ZeroMemory(&dd, sizeof(dd)); dd.cb = sizeof(dd);
	for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
		if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
		std::string gpuName = dd.DeviceString;
		if (!gpuName.empty() && std::find(gpuList.begin(), gpuList.end(), gpuName) == gpuList.end()) {
			gpuList.push_back(gpuName);
		}
	}
	if (gpuList.empty()) gpuList.push_back("Unknown GPU");
	return gpuList;
}

double GetRamUsage() {
	MEMORYSTATUSEX memInfo; memInfo.dwLength = sizeof(MEMORYSTATUSEX); GlobalMemoryStatusEx(&memInfo);
	return ((double)(memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (double)memInfo.ullTotalPhys * 100.0);
}

class CpuUsage {
private:
	ULARGE_INTEGER lastIdleTime, lastKernelTime, lastUserTime;
	void filetime_to_ularge(const FILETIME& ft, ULARGE_INTEGER& ul) {
		ul.LowPart = ft.dwLowDateTime; ul.HighPart = ft.dwHighDateTime;
	}
public:
	CpuUsage() {
		FILETIME idleTime, kernelTime, userTime;
		GetSystemTimes(&idleTime, &kernelTime, &userTime);
		filetime_to_ularge(idleTime, lastIdleTime);
		filetime_to_ularge(kernelTime, lastKernelTime);
		filetime_to_ularge(userTime, lastUserTime);
	}
	double GetCpuUsage() {
		FILETIME idleTime, kernelTime, userTime;
		GetSystemTimes(&idleTime, &kernelTime, &userTime);
		ULARGE_INTEGER idle, kernel, user;
		filetime_to_ularge(idleTime, idle); filetime_to_ularge(kernelTime, kernel); filetime_to_ularge(userTime, user);
		ULONGLONG sysIdleDiff = idle.QuadPart - lastIdleTime.QuadPart;
		ULONGLONG sysKernelDiff = kernel.QuadPart - lastKernelTime.QuadPart;
		ULONGLONG sysUserDiff = user.QuadPart - lastUserTime.QuadPart;
		lastIdleTime = idle; lastKernelTime = kernel; lastUserTime = user;
		ULONGLONG totalSys = sysKernelDiff + sysUserDiff;
		if (totalSys == 0) return 0.0;
		double cpuUsage = (1.0 - ((double)sysIdleDiff / (double)totalSys)) * 100.0;
		return cpuUsage < 0.0 ? 0.0 : (cpuUsage > 100.0 ? 100.0 : cpuUsage);
	}
};

CpuUsage cpuUsage;

void StartServer() {
	crow::SimpleApp app;

	std::thread bgmThread(BGMThreadFunc);
	bgmThread.detach();

	PlayBGM();

	CROW_ROUTE(app, "/assets/mascot.jpg")([]() {
		std::string imgData = GetResourceData(IDR_MASCOT1_IMG, MAKEINTRESOURCEA(10));
		crow::response res(imgData); res.set_header("Content-Type", "image/jpeg"); return res;
		});

	CROW_ROUTE(app, "/assets/mascot2.jpg")([]() {
		std::string imgData = GetResourceData(IDR_MASCOT2_IMG, MAKEINTRESOURCEA(10));
		crow::response res(imgData); res.set_header("Content-Type", "image/jpeg"); return res;
		});

	CROW_ROUTE(app, "/api/stats")([]() {
		double gpuUsage = 0.0, gpuTemp = 0.0; GetGpuStats(gpuUsage, gpuTemp);
		crow::json::wvalue res;
		res["ramUsage"] = GetRamUsage();
		res["cpuUsage"] = cpuUsage.GetCpuUsage();
		res["gpuUsage"] = gpuUsage;
		res["gpuTemp"] = gpuTemp;
		res["mode"] = (int)g_currentMode.load();
		res["isMuted"] = g_isMuted.load();
		res["volume"] = g_bgmVolume.load();
		return res;
		});

	CROW_ROUTE(app, "/api/specs")([]() {
		crow::json::wvalue res;
		res["cpuName"] = GetRealCpuName();
		res["totalRam"] = GetRealTotalRam();
		res["gpuNames"] = GetRealGpuName();
		return res;
		});

	CROW_ROUTE(app, "/api/set_mode")([](const crow::request& req) {
		if (req.url_params.get("m")) {
			std::string m = req.url_params.get("m");
			if (m == "window") SetAppMode(MODE_WINDOW);
			else if (m == "overlay") SetAppMode(MODE_OVERLAY);
		}
		return crow::response(200);
		});

	CROW_ROUTE(app, "/api/toggle_audio")([]() {
		if (g_isMuted) ResumeBGM();
		else StopBGM();
		crow::json::wvalue res;
		res["isMuted"] = g_isMuted.load();
		return res;
		});

	CROW_ROUTE(app, "/api/set_volume")([](const crow::request& req) {
		if (req.url_params.get("v")) {
			int vol = std::stoi(req.url_params.get("v"));
			SetBGMVolume(vol);
		}
		return crow::response(200);
		});

	CROW_ROUTE(app, "/")([]() {
		auto page = crow::response(R"rawhtml(
			<!DOCTYPE html>
			<html lang="en">
			<head>
				<meta charset="UTF-8">
				<meta name="viewport" content="width=device-width, initial-scale=1.0">
				<title>System Monitor</title>
				<style>
					* { box-sizing: border-box; margin: 0; padding: 0; }
					html, body {
						width: 100vw; height: 100vh; overflow: hidden;
						background-color: #0d0b12; font-family: 'Consolas', 'Segoe UI', sans-serif;
						color: #f1f5f9; user-select: none; -webkit-user-select: none;
					}
					#window-ui {
						width: 100%; height: 100%; display: flex; align-items: center; padding-left: 60px;
						background: url('/assets/mascot.jpg') no-repeat center center / cover;
						transition: background-image 0.5s ease; position: relative;
					}
					#window-ui .overlay {
						position: absolute; top: 0; left: 0; width: 100%; height: 100%;
						background: linear-gradient(90deg, rgba(16, 28, 20, 0.92) 0%, rgba(16, 28, 20, 0.65) 45%, rgba(16, 28, 20, 0.1) 100%);
						transition: background 0.5s ease; z-index: 1;
					}
					#window-ui .container { position: relative; z-index: 2; max-width: 500px; width: 100%; }
					.header { margin-bottom: 28px; border-bottom: 2px solid rgba(138, 190, 140, 0.4); padding-bottom: 12px; }
					.header h1 { font-size: 2.3rem; color: #a4e4b0; font-weight: 700; letter-spacing: 0.5px; }
					.header p { font-size: 0.95rem; color: #c4d6c8; margin-top: 4px; }
					.grid-container { display: grid; grid-template-columns: repeat(2, 1fr); gap: 18px; }
					.card {
						background: rgba(18, 30, 22, 0.68); border: 1px solid rgba(138, 190, 140, 0.3);
						backdrop-filter: blur(16px); border-radius: 16px; padding: 22px 24px;
						box-shadow: 0 10px 30px rgba(0, 0, 0, 0.45); transition: all 0.3s ease;
					}
					.card.full-width { grid-column: span 2; }
					.card h3 { font-size: 0.8rem; color: #8ba993; text-transform: uppercase; letter-spacing: 1.2px; margin-bottom: 8px; }
					.card .value { font-size: 1.8rem; font-weight: 700; color: #8ae0a0; }
					.card .unit { font-size: 1rem; color: #c4d6c8; font-weight: 400; }
					.controls-bar { position: fixed; bottom: 35px; left: 60px; z-index: 10; display: flex; align-items: center; gap: 12px; }
					.switch-btn {
						background: rgba(18, 30, 22, 0.85); border: 1px solid rgba(138, 190, 140, 0.4);
						color: #8ae0a0; padding: 12px 22px; border-radius: 24px; cursor: pointer;
						font-size: 0.88rem; font-weight: 600; backdrop-filter: blur(12px); transition: all 0.2s ease;
					}
					.switch-btn:hover { transform: translateY(-2px); background: rgba(138, 190, 140, 0.2); }
					.volume-box {
						display: flex; align-items: center; gap: 8px;
						background: rgba(18, 30, 22, 0.85); border: 1px solid rgba(138, 190, 140, 0.4);
						padding: 8px 16px; border-radius: 24px; backdrop-filter: blur(12px);
					}
					.volume-slider {
						-webkit-appearance: none; width: 90px; height: 5px; border-radius: 5px;
						background: rgba(138, 190, 140, 0.3); outline: none; transition: background 0.2s;
					}
					.volume-slider::-webkit-slider-thumb {
						-webkit-appearance: none; appearance: none; width: 14px; height: 14px;
						border-radius: 50%; background: #8ae0a0; cursor: pointer; transition: transform 0.1s;
					}
					.volume-slider::-webkit-slider-thumb:hover { transform: scale(1.2); }
					.volume-label { font-size: 0.8rem; color: #8ae0a0; min-width: 38px; font-weight: 600; }
					.version-tag {
						position: fixed; bottom: 25px; right: 30px; z-index: 10; font-size: 0.85rem;
						color: rgba(138, 224, 160, 0.5); text-decoration: none; padding: 6px 12px;
						border-radius: 12px; background: rgba(0, 0, 0, 0.25); border: 1px solid rgba(138, 190, 140, 0.15);
					}
					
					/* ==== PAGE 2 ==== */
					body.specs-mode #window-ui { background-image: url('/assets/mascot2.jpg'); }
					body.specs-mode #window-ui .overlay { background: linear-gradient(90deg, rgba(28, 16, 35, 0.93) 0%, rgba(28, 16, 35, 0.7) 45%, rgba(28, 16, 35, 0.15) 100%); }
					body.specs-mode #window-ui .header { border-bottom-color: rgba(190, 140, 240, 0.4); }
					body.specs-mode #window-ui .header h1 { color: #d6a4e4; }
					body.specs-mode #window-ui .header p { color: #d0c4d6; }
					body.specs-mode #window-ui .card { background: rgba(28, 16, 35, 0.72); border-color: rgba(190, 140, 240, 0.3); }
					body.specs-mode #window-ui .card h3 { color: #a98ba9; }
					body.specs-mode #window-ui .card .value { color: #d68ae0; }
					body.specs-mode .switch-btn, body.specs-mode .volume-box { background: rgba(28, 16, 35, 0.85); border-color: rgba(190, 140, 240, 0.4); color: #d68ae0; }
					body.specs-mode .volume-slider::-webkit-slider-thumb { background: #d68ae0; }
					body.specs-mode .volume-label { color: #d68ae0; }
					body.specs-mode .switch-btn:hover { background: rgba(190, 140, 240, 0.2); }
					body.specs-mode .version-tag { border-color: rgba(190, 140, 240, 0.2); color: rgba(214, 138, 224, 0.6); }

					/* ==== OVERLAY MODE ==== */
					#overlay-ui {
						display: none; width: 100vw; height: 100vh;
						background: rgba(16, 28, 20, 0.92); border: 1.5px solid #8ae0a0;
						border-radius: 8px; padding: 8px 12px; position: relative;
						box-sizing: border-box; cursor: move; -webkit-app-region: drag;
					}
					.osd-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 4px 12px; height: 100%; align-content: center; }
					.osd-item { display: flex; align-items: center; gap: 6px; font-weight: bold; }
					.osd-lbl { font-size: 0.72rem; color: #8ae0a0; }
					.osd-val { font-size: 0.85rem; color: #ffffff; }
					.restore-btn {
						position: absolute; top: 4px; right: 6px;
						background: rgba(200, 75, 50, 0.85); color: #fff;
						border: none; border-radius: 4px; font-size: 0.65rem;
						padding: 2px 6px; cursor: pointer; z-index: 99;
						opacity: 0; transition: opacity 0.2s ease; -webkit-app-region: no-drag;
					}
					#overlay-ui:hover .restore-btn { opacity: 1; }
					body.overlay-mode #window-ui { display: none !important; }
					body.overlay-mode #overlay-ui { display: flex !important; }
				</style>
			</head>
			<body oncontextmenu="return false;" onmousedown="handleDrag(event)">
				<div id="window-ui">
					<div class="overlay"></div>
					<div class="container">
						<div class="header">
							<h1 id="panel-title">System Monitor</h1>
							<p id="panel-subtitle">Live Hardware Performance Dashboard</p>
						</div>
						<div class="grid-container" id="cards-grid">
							<div class="card"><h3>CPU</h3><div class="value"><span id="cpu">--</span> <span class="unit">%</span></div></div>
							<div class="card"><h3>RAM</h3><div class="value"><span id="ram">--</span> <span class="unit">%</span></div></div>
							<div class="card"><h3>GPU</h3><div class="value"><span id="gpu">--</span> <span class="unit">%</span></div></div>
							<div class="card"><h3>TEMP</h3><div class="value"><span id="gputemp">--</span> <span class="unit">&#176;C</span></div></div>
						</div>
					</div>
					<div class="controls-bar">
						<button class="switch-btn" id="btn-toggle" onclick="toggleView()">&#x21BB; Specs</button>
						<button class="switch-btn" id="btn-audio" onclick="toggleAudio()">&#128066; Mute</button>
						<div class="volume-box">
							<span style="font-size: 0.9rem;">&#128066;</span>
							<input type="range" id="vol-slider" class="volume-slider" min="0" max="100" value="100" oninput="changeVolume(this.value)">
							<span id="vol-txt" class="volume-label">100%</span>
						</div>
						<button class="switch-btn" onclick="setMode('overlay')">&#128159; Overlay</button>
					</div>
					<a href="https://github.com/xiaokuai0915/System-Monitor-App" target="_blank" class="version-tag">v1.2.0</a>
				</div>

				<div id="overlay-ui">
					<button class="restore-btn" onclick="setMode('window')">&#128450; Restore</button>
					<div class="osd-grid">
						<div class="osd-item"><span class="osd-lbl">CPU</span><span class="osd-val" id="ov-cpu">--</span>%</div>
						<div class="osd-item"><span class="osd-lbl">RAM</span><span class="osd-val" id="ov-ram">--</span>%</div>
						<div class="osd-item"><span class="osd-lbl">GPU</span><span class="osd-val" id="ov-gpu">--</span>%</div>
						<div class="osd-item"><span class="osd-lbl">TEMP</span><span class="osd-val" id="ov-gputemp">--</span>&#176;C</div>
					</div>
				</div>

				<script>
					let isSpecsView = false;
					let cachedSpecs = null;
					let currentMode = 'window';

					function handleDrag(e) {
						if (currentMode === 'overlay' && e.button === 0 && e.target.tagName !== 'BUTTON') {
							if (window.nativeDrag) window.nativeDrag();
						}
					}

					function setMode(mode) {
						fetch('/api/set_mode?m=' + mode).then(() => {
							currentMode = mode;
							applyModeStyles(mode);
						});
					}

					function applyModeStyles(mode) {
						document.body.classList.remove('overlay-mode');
						if (mode === 'overlay') document.body.classList.add('overlay-mode');
					}

					function toggleAudio() {
						fetch('/api/toggle_audio')
							.then(res => res.json())
							.then(data => {
								const btn = document.getElementById('btn-audio');
								if (data.isMuted) btn.innerHTML = '&#128067; Unmute';
								else btn.innerHTML = '&#128066; Mute';
							});
					}

					function changeVolume(val) {
						document.getElementById('vol-txt').innerText = val + '%';
						fetch('/api/set_volume?v=' + val);
					}

					function toggleView() {
						if (currentMode !== 'window') return;
						isSpecsView = !isSpecsView;
						const body = document.body;
						const grid = document.getElementById('cards-grid');
						const btn = document.getElementById('btn-toggle');
						const title = document.getElementById('panel-title');
						const subtitle = document.getElementById('panel-subtitle');

						if (isSpecsView) {
							body.classList.add('specs-mode');
							title.innerText = "System Specs";
							subtitle.innerText = "Hardware Configuration Details";
							btn.innerHTML = "&#x21BB; Performance";
							if (cachedSpecs) renderSpecsCards(cachedSpecs);
							else fetch('/api/specs').then(res => res.json()).then(data => { cachedSpecs = data; renderSpecsCards(data); });
						} else {
							body.classList.remove('specs-mode');
							title.innerText = "System Monitor";
							subtitle.innerText = "Live Hardware Performance Dashboard";
							btn.innerHTML = "&#x21BB; Specs";
							grid.innerHTML = `
								<div class="card"><h3>CPU</h3><div class="value"><span id="cpu">--</span> <span class="unit">%</span></div></div>
								<div class="card"><h3>RAM</h3><div class="value"><span id="ram">--</span> <span class="unit">%</span></div></div>
								<div class="card"><h3>GPU</h3><div class="value"><span id="gpu">--</span> <span class="unit">%</span></div></div>
								<div class="card"><h3>TEMP</h3><div class="value"><span id="gputemp">--</span> <span class="unit">&#176;C</span></div></div>
							`;
							updateStats();
						}
					}

					function renderSpecsCards(data) {
						const grid = document.getElementById('cards-grid');
						let html = `<div class="card full-width"><h3>Processor (CPU)</h3><div class="value" style="font-size: 1.25rem;">${data.cpuName}</div></div>`;
						if (data.gpuNames && data.gpuNames.length > 0) {
							data.gpuNames.forEach((gpu, index) => {
								let title = data.gpuNames.length > 1 ? `Graphics Card (GPU ${index + 1})` : `Graphics Card (GPU)`;
								html += `<div class="card full-width"><h3>${title}</h3><div class="value" style="font-size: 1.25rem;">${gpu}</div></div>`;
							});
						}
						html += `<div class="card full-width"><h3>Total Memory</h3><div class="value">${data.totalRam}</div></div>`;
						grid.innerHTML = html;
					}

					function updateStats() {
						fetch('/api/stats')
							.then(res => res.json())
							.then(data => {
								const cpu = data.cpuUsage.toFixed(1);
								const ram = data.ramUsage.toFixed(1);
								const gpu = data.gpuUsage.toFixed(1);
								const temp = data.gpuTemp.toFixed(1);

								if (!isSpecsView) {
									if (document.getElementById('cpu')) document.getElementById('cpu').innerText = cpu;
									if (document.getElementById('ram')) document.getElementById('ram').innerText = ram;
									if (document.getElementById('gpu')) document.getElementById('gpu').innerText = gpu;
									if (document.getElementById('gputemp')) document.getElementById('gputemp').innerText = temp;
								}

								document.getElementById('ov-cpu').innerText = cpu;
								document.getElementById('ov-ram').innerText = ram;
								document.getElementById('ov-gpu').innerText = gpu;
								document.getElementById('ov-gputemp').innerText = temp;

								const audioBtn = document.getElementById('btn-audio');
								if (data.isMuted) {
									audioBtn.innerHTML = '&#128067; Unmute';
								} else {
									audioBtn.innerHTML = '&#128066; Mute';
								}

								const slider = document.getElementById('vol-slider');
								if (document.activeElement !== slider) {
									slider.value = data.volume;
									document.getElementById('vol-txt').innerText = data.volume + '%';
								}

								if (data.mode === 0 && currentMode !== 'window') { currentMode = 'window'; applyModeStyles('window'); }
								else if (data.mode === 1 && currentMode !== 'overlay') { currentMode = 'overlay'; applyModeStyles('overlay'); }
							})
							.catch(err => console.error('Fetch Error:', err));
					}

					setInterval(updateStats, 500);
					updateStats();
				</script>
			</body>
			</html>
		)rawhtml");
		page.set_header("Content-Type", "text/html");
		return page;
		});

	app.port(18080).multithreaded().run();
}

int main() {
	std::thread serverThread(StartServer);
	serverThread.detach();

	webview::webview w(false, nullptr);
	w.set_title("System Hardware Monitor");
	w.set_size(1600, 900, WEBVIEW_HINT_NONE);

	w.bind("nativeDrag", [](const std::string& req) -> std::string {
		if (g_hwndWebview) {
			ReleaseCapture();
			SendMessageA(g_hwndWebview, WM_SYSCOMMAND, 0xF012, 0);
		}
		return "";
		});

	w.navigate("http://127.0.0.1:18080");

	g_hwndWebview = FindWindowA(nullptr, "System Hardware Monitor");
	if (g_hwndWebview) {
		g_oldWndProc = (WNDPROC)SetWindowLongPtrA(g_hwndWebview, GWLP_WNDPROC, (LONG_PTR)SubclassWndProc);

		HICON hIcon = (HICON)LoadImageA(
			GetModuleHandle(NULL),
			MAKEINTRESOURCEA(IDI_APP_ICON),
			IMAGE_ICON,
			GetSystemMetrics(SM_CXICON),
			GetSystemMetrics(SM_CYICON),
			LR_DEFAULTCOLOR
		);
		if (hIcon) {
			SendMessage(g_hwndWebview, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
			SendMessage(g_hwndWebview, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
		}
	}
	w.run();

	g_stopBgmThread = true;
	PlaySoundA(NULL, 0, SND_ASYNC);
	ExitProcess(0);

	return 0;
}