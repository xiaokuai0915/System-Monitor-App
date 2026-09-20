#define _CRT_SECURE_NO_WARNINGS //ignore deprecated warnings for functions like sprintf, strcpy, etc.
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup") // tell the compiler this is a windows application and not a console application, but still use main() as the entry point

// both window libaries that used for capture pc details
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h> // for PlaySound
#pragma comment(lib, "winmm.lib") // auto link winmm.lib for PlaySound

#include "resource.h" // load resources like images and sounds
#include <thread> // for running the web server in a separate thread
#include <sysinfoapi.h> // for getting RAM usage
#include <processthreadsapi.h> // for getting CPU usage

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

// tell crow to use standalone ASIO (no Boost dependency)
#define ASIO_STANDALONE
#define CROW_MAIN
#include "crow.h"

// tell webview to use WinAPI backend
#define WEBVIEW_WINAPI
#include "webview.h"

// read resouce data from the .exe ram that was compiled with the resources
std::string GetResourceData(int resourceId, const char* resourceType) {
	HMODULE hModule = GetModuleHandle(NULL); // get the handle to the current module (the .exe file)
	HRSRC hRes = FindResourceA(hModule, MAKEINTRESOURCEA(resourceId), resourceType);
	if (!hRes) return ""; // if the resource is not found, return an empty string
	HGLOBAL hData = LoadResource(hModule, hRes); // load the resource into memory
	DWORD size = SizeofResource(hModule, hRes); // get the size of the resource
	char* data = (char*)LockResource(hData); // lock the resource in memory and get a pointer to it
	if (!data || size == 0) return ""; // if the resource is empty, return an empty string
	return std::string(data, size);
}

void* __stdcall ADL_Main_Memory_Alloc(int iSize) {
	return malloc(iSize);
}

struct ADLTemperature {
	int iSize;
	int iTemperature;
};

struct ADLPMActivity {
	int iSize;
	int iEngineClock;
	int iMemoryClock;
	int iVddc;
	int iActivityPercent;
	int iCurrentPerformanceLevel;
	int iCurrentBusSpeed;
	int iCurrentBusLanes;
	int iMaximumBusLanes;
	int iReserved;
};

struct AdapterInfo {
	int iSize;
	int iAdapterIndex;
	char strUDID[256];
	int iBusNumber;
	int iDeviceNumber;
	int iFunctionNumber;
	int iVendorID;
	char strAdapterName[256];
	char strDisplayName[256];
	int iPresent;
	int iExist;
	char strDriverPath[256];
	char strDriverPathExt[256];
	char strPNPString[256];
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
					// 1002 is AMD Vendor ID
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

struct nvmlUtilization_t { // structure to hold GPU utilization rates
	unsigned int gpu;
	unsigned int memory;
};

// NVML temperature sensor types
typedef int (*nvmlInit_t)(); // function pointer type for nvmlInit
typedef int (*nvmlShutdown_t)(); // function pointer type for nvmlShutdown
typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int index, void** device); // function pointer type for nvmlDeviceGetHandleByIndex
typedef int (*nvmlDeviceGetTemperature_t)(void* device, int sensorType, unsigned int* temp); // function pointer type for nvmlDeviceGetTemperature
typedef int (*nvmlDeviceGetUtilizationRates_t)(void* device, nvmlUtilization_t* rates); // function pointer type for nvmlDeviceGetUtilizationRates

void GetGpuStats(double& usageOut, double& tempOut) {
	usageOut = 0.0;
	tempOut = 0.0;

	// try NVIDA NVML first
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
				if (nvmlGetTemp && nvmlGetTemp(device, 0, &temp) == 0) {
					tempOut = (double)temp;
				}

				nvmlUtilization_t util = { 0 };
				if (nvmlGetUtil && nvmlGetUtil(device, &util) == 0) {
					usageOut = (double)util.gpu;
				}

				if (nvmlShutdown) nvmlShutdown();
				FreeLibrary(hNvml);
				return; // capture NVIDIA card sucess
			}
			if (nvmlShutdown) nvmlShutdown();
		}
		FreeLibrary(hNvml);
	}

	// if cannot dected nvidia/NVML capture failed then try AMD DL
	GetAmdGpuStats(usageOut, tempOut);
}

std::string GetRealCpuName() { // function to get the real CPU name from the Windows registry
	HKEY hKey; // handle to the registry key
	char cpuName[256] = "Unknown CPU"; // buffer to hold the CPU name
	DWORD bufSize = sizeof(cpuName); // size of the buffer
	// Open the registry key for the CPU name
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { // if the key is opened successfully
		RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &bufSize); // query the value of the CPU name
		RegCloseKey(hKey);
	}
	return std::string(cpuName);
}

std::string GetRealTotalRam() { // function to get the total RAM size in GB
	MEMORYSTATUSEX memInfo; // structure to hold memory status information
	memInfo.dwLength = sizeof(MEMORYSTATUSEX); // set the size of the structure
	GlobalMemoryStatusEx(&memInfo); // get the memory status information
	double totalGB = (double)memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0); // convert total physical memory from bytes to gigabytes
	char buf[32]; // buffer to hold the formatted string
	snprintf(buf, sizeof(buf), "%.1f GB", totalGB); // format the total RAM size to one decimal place and store it in the buffer
	return std::string(buf);
}

std::vector<std::string> GetRealGpuName() { // function to get the real GPU name using EnumDisplayDevices
	std::vector<std::string> gpuList;
	DISPLAY_DEVICEA dd;
	ZeroMemory(&dd, sizeof(dd));
	dd.cb = sizeof(dd);

	for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
		if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;

		std::string gpuName = dd.DeviceString;

		if (!gpuName.empty() && std::find(gpuList.begin(), gpuList.end(), gpuName) == gpuList.end()) {
			gpuList.push_back(gpuName);
		}
	}

	if (gpuList.empty()) {
		gpuList.push_back("Unknown GPU");
	}
	return gpuList;
}

double GetRamUsage() {
	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX); // set the size of the structure
	GlobalMemoryStatusEx(&memInfo); // get the memory status information
	DWORDLONG totalPhysMem = memInfo.ullTotalPhys; // total physical memory in bytes
	DWORDLONG physMemUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys; // used physical memory in bytes
	return ((double)physMemUsed / (double)totalPhysMem * 100.0); // calculate the RAM usage percentage
}

class CpuUsage {
private:
	ULARGE_INTEGER lastIdleTime, lastKernelTime, lastUserTime; // store the last recorded CPU times
	void filetime_to_ularge(const FILETIME& ft, ULARGE_INTEGER& ul) { // convert FILETIME to ULARGE_INTEGER
		ul.LowPart = ft.dwLowDateTime; // set the low part of the ULARGE_INTEGER
		ul.HighPart = ft.dwHighDateTime; // set the high part of the ULARGE_INTEGER
	}
public:
	CpuUsage() {
		FILETIME idleTime, kernelTime, userTime; // get the initial CPU times
		GetSystemTimes(&idleTime, &kernelTime, &userTime); // get the system times for idle, kernel, and user
		filetime_to_ularge(idleTime, lastIdleTime); // convert and store the last idle time
		filetime_to_ularge(kernelTime, lastKernelTime); // convert and store the last kernel time
		filetime_to_ularge(userTime, lastUserTime); // convert and store the last user time
	}
	double GetCpuUsage() {
		FILETIME idleTime, kernelTime, userTime;
		GetSystemTimes(&idleTime, &kernelTime, &userTime);

		ULARGE_INTEGER idle, kernel, user; // convert the FILETIME values to ULARGE_INTEGER for calculations
		filetime_to_ularge(idleTime, idle);
		filetime_to_ularge(kernelTime, kernel);
		filetime_to_ularge(userTime, user);

		ULONGLONG sysIdleDiff = idle.QuadPart - lastIdleTime.QuadPart; // calculate the difference in idle time since the last measurement
		ULONGLONG sysKernelDiff = kernel.QuadPart - lastKernelTime.QuadPart; // calculate the difference in kernel time since the last measurement
		ULONGLONG sysUserDiff = user.QuadPart - lastUserTime.QuadPart; // calculate the difference in user time since the last measurement

		lastIdleTime = idle;
		lastKernelTime = kernel;
		lastUserTime = user;

		ULONGLONG totalSys = sysKernelDiff + sysUserDiff; // calculate the total system time since the last measurement
		if (totalSys == 0) return 0.0;

		double cpuUsage = (1.0 - ((double)sysIdleDiff / (double)totalSys)) * 100.0; // calculate the CPU usage percentage
		return cpuUsage < 0.0 ? 0.0 : (cpuUsage > 100.0 ? 100.0 : cpuUsage); // clamp the CPU usage percentage between 0 and 100
	}
};

CpuUsage cpuUsage; // create a global instance of the CpuUsage class to track CPU usage over time

void PlayRandomVoiceLine() {
	static const int voiceIds[] = { IDR_VOICE, IDR_VOICE2, IDR_VOICE3 }; // array of resource IDs for the voice lines

	static std::random_device rd; // obtain a random number from hardware
	static std::mt19937 gen(rd()); // seed the generator
	std::uniform_int_distribution<> dis(0, 2); // random generate 0 to 2

	int selectedVoice = voiceIds[dis(gen)];

	PlaySoundA(
		MAKEINTRESOURCEA(selectedVoice),
		GetModuleHandle(NULL),
		SND_RESOURCE | SND_ASYNC | SND_NOSTOP
	);
}

void StartServer() {
	crow::SimpleApp app; // create a simple Crow application

	CROW_ROUTE(app, "/assets/mascot.jpg") // route to serve the mascot image
		([]() {
		std::string imgData = GetResourceData(IDR_MASCOT1_IMG, MAKEINTRESOURCEA(10)); // RT_RCDATA = 10
		crow::response res(imgData); // create a response with the image data
		res.set_header("Content-Type", "image/jpeg"); // set the content type header to image/jpeg
		return res;
			});

	CROW_ROUTE(app, "/assets/mascot2.jpg")
		([]() {
		std::string imgData = GetResourceData(IDR_MASCOT2_IMG, MAKEINTRESOURCEA(10)); // RT_RCDATA = 10
		crow::response res(imgData);
		res.set_header("Content-Type", "image/jpeg");
		return res;
			});

	CROW_ROUTE(app, "/api/stats") // route to serve the system stats as JSON
		([]() {
		double gpuUsage = 0.0, gpuTemp = 0.0;
		GetGpuStats(gpuUsage, gpuTemp);
		crow::json::wvalue res; // create a JSON response object
		res["ramUsage"] = GetRamUsage();
		res["cpuUsage"] = cpuUsage.GetCpuUsage();
		res["gpuUsage"] = gpuUsage;
		res["gpuTemp"] = gpuTemp;
		return res; // return the JSON response
			});

	CROW_ROUTE(app, "/api/specs") // route to serve the system specs as JSON
		([]() {
		crow::json::wvalue res;
		res["cpuName"] = GetRealCpuName();
		res["totalRam"] = GetRealTotalRam();
		res["gpuNames"] = GetRealGpuName();
		return res;
			});
	
	CROW_ROUTE(app, "/api/play_voice")
		([]() {
		PlayRandomVoiceLine();
		return crow::response(200);
			});

	CROW_ROUTE(app, "/") // route to serve the main HTML page
		([]() {
		// return the HTML page as a raw string
		auto page = crow::response(R"rawhtml(
			<!DOCTYPE html>
			<html lang="en">
			<head>
				<meta charset="UTF-8">
				<meta name="viewport" content="width=device-width, initial-scale=1.0">
				<title>System Monitor</title>
				<style>
					* { box-sizing: border-box; margin: 0; padding: 0; }
					body {
						font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
						height: 100vh;
						width: 100vw;
						display: flex;
						align-items: center;
						padding-left: 60px;
						background: url('/assets/mascot.jpg') no-repeat center center / cover;
						transition: background-image 0.6s ease-in-out;
						color: #f1f5f9;
						overflow: hidden;
						position: relative;
						user-select: none; 
						-webkit-user-select: none;
					}

					.overlay {
						position: absolute;
						top: 0; left: 0; width: 100%; height: 100%;
						background: linear-gradient(90deg, rgba(14, 10, 18, 0.9) 0%, rgba(14, 10, 18, 0.6) 45%, rgba(14, 10, 18, 0.1) 100%);
						z-index: 1;
						transition: background 0.6s ease-in-out;
					}

					.container {
						position: relative;
						z-index: 2;
						max-width: 500px;
						width: 100%;
					}

					.header {
						margin-bottom: 28px;
						border-bottom: 2px solid rgba(255, 120, 50, 0.4);
						padding-bottom: 12px;
						transition: border-color 0.6s ease;
					}
					.header h1 { font-size: 2.3rem; color: #ff9d66; font-weight: 700; transition: color 0.6s ease; cursor: default; }
					.header p { font-size: 0.95rem; color: #cbd5e1; margin-top: 4px; transition: color 0.6s ease; cursor: default; }

					.grid-container {
						display: grid;
						grid-template-columns: repeat(2, 1fr);
						gap: 18px;
					}

					.card {
						background: rgba(22, 16, 28, 0.65);
						border: 1px solid rgba(255, 120, 50, 0.25);
						backdrop-filter: blur(16px);
						border-radius: 16px;
						padding: 22px 24px;
						box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4);
						transition: all 0.3s ease;
						cursor: default;
					}
					.card:hover {
						transform: translateY(-3px); 
						box-shadow: 0 12px 35px rgba(0, 0, 0, 0.5);
					}
					.card.full-width { grid-column: span 2; }

					.card h3 { font-size: 0.8rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 1.2px; margin-bottom: 8px; transition: color 0.6s ease; }
					.card .value { font-size: 1.8rem; font-weight: 700; color: #ffab76; transition: color 0.6s ease; }
					.card .unit { font-size: 1rem; color: #cbd5e1; font-weight: 400; }

					body.specs-mode {
						background-image: url('/assets/mascot2.jpg');
					}
					body.specs-mode .overlay {
						background: linear-gradient(90deg, rgba(10, 22, 40, 0.85) 0%, rgba(10, 22, 40, 0.5) 50%, rgba(10, 22, 40, 0.05) 100%);
					}
					body.specs-mode .header { border-bottom-color: rgba(56, 189, 248, 0.4); }
					body.specs-mode .header h1 { color: #38bdf8; }
					body.specs-mode .header p { color: #bae6fd; }
					body.specs-mode .card { background: rgba(12, 28, 48, 0.65); border-color: rgba(56, 189, 248, 0.25); }
					body.specs-mode .card h3 { color: #7dd3fc; }
					body.specs-mode .card .value { color: #38bdf8; }

					.switch-btn {
						position: fixed;
						bottom: 35px;
						left: 60px;
						z-index: 10;
						background: rgba(22, 16, 28, 0.8);
						border: 1px solid rgba(255, 120, 50, 0.4);
						color: #ffab76;
						padding: 12px 26px;
						border-radius: 24px;
						cursor: pointer;
						font-size: 0.88rem;
						font-weight: 600;
						backdrop-filter: blur(12px);
						box-shadow: 0 4px 20px rgba(0, 0, 0, 0.4);
						transition: all 0.3s ease;
					}
					.switch-btn:hover {
						transform: translateY(-2px) scale(1.03);
						background: rgba(255, 120, 50, 0.15);
					}
					body.specs-mode .switch-btn {
						background: rgba(12, 28, 48, 0.8);
						border-color: rgba(56, 189, 248, 0.4);
						color: #7dd3fc;
					}
					body.specs-mode .switch-btn:hover { background: rgba(56, 189, 248, 0.15); }
				</style>
			</head>
			<body oncontextmenu="return false;">
				<div class="overlay"></div>

				<div class="container">
					<div class="header">
						<h1 id="panel-title">System Monitor</h1>
						<p id="panel-subtitle">Live Hardware Performance Dashboard</p>
					</div>

					<div class="grid-container" id="cards-grid">
						<div class="card">
							<h3>CPU Usage</h3>
							<div class="value"><span id="cpu">--</span> <span class="unit">%</span></div>
						</div>
						<div class="card">
							<h3>RAM Usage</h3>
							<div class="value"><span id="ram">--</span> <span class="unit">%</span></div>
						</div>
						<div class="card">
							<h3>GPU Usage</h3>
							<div class="value"><span id="gpu">--</span> <span class="unit">%</span></div>
						</div>
						<div class="card">
							<h3>GPU Temp</h3>
							<div class="value"><span id="gputemp">--</span> <span class="unit">&#176;C</span></div>
						</div>
					</div>
				</div>

				<button class="switch-btn" id="btn-toggle" onclick="toggleView()">
					&#x21BB; Show System Hardware Specs
				</button>

				<script>
					let isSpecsView = false;
					let cachedSpecs = null;

					function toggleView() {
						fetch('/api/play_voice').catch(err => console.error(err));
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
							btn.innerHTML = "&#x21BB; Show Live Performance";

							if (cachedSpecs) {
								renderSpecsCards(cachedSpecs);
							} else {
								fetch('/api/specs')
									.then(res => res.json())
									.then(data => {
										cachedSpecs = data;
										renderSpecsCards(data);
									});
							}
						} else {
							body.classList.remove('specs-mode');
							title.innerText = "System Monitor";
							subtitle.innerText = "Live Hardware Performance Dashboard";
							btn.innerHTML = "&#x21BB; Show System Hardware Specs";

							grid.innerHTML = `
								<div class="card">
									<h3>CPU Usage</h3>
									<div class="value"><span id="cpu">--</span> <span class="unit">%</span></div>
								</div>
								<div class="card">
									<h3>RAM Usage</h3>
									<div class="value"><span id="ram">--</span> <span class="unit">%</span></div>
								</div>
								<div class="card">
									<h3>GPU Usage</h3>
									<div class="value"><span id="gpu">--</span> <span class="unit">%</span></div>
								</div>
								<div class="card">
									<h3>GPU Temp</h3>
									<div class="value"><span id="gputemp">--</span> <span class="unit">&#176;C</span></div>
								</div>
							`;
							updateStats();
						}
					}

					function renderSpecsCards(data) {
						const grid = document.getElementById('cards-grid');
						
						let html = `
							<div class="card full-width">
								<h3>Processor (CPU)</h3>
								<div class="value" style="font-size: 1.25rem;">${data.cpuName}</div>
							</div>
						`;

						if (data.gpuNames && data.gpuNames.length > 0) {
							data.gpuNames.forEach((gpu, index) => {
								let title = data.gpuNames.length > 1 ? `Graphics Card (GPU ${index + 1})` : `Graphics Card (GPU)`;
								html += `
									<div class="card full-width">
										<h3>${title}</h3>
										<div class="value" style="font-size: 1.25rem;">${gpu}</div>
									</div>
								`;
							});
						}

						html += `
							<div class="card full-width">
								<h3>Total Memory</h3>
								<div class="value">${data.totalRam}</div>
							</div>
						`;

						grid.innerHTML = html;
					}

					function updateStats() {
						if (isSpecsView) return;

						fetch('/api/stats')
							.then(res => res.json())
							.then(data => {
								if (document.getElementById('cpu')) document.getElementById('cpu').innerText = data.cpuUsage.toFixed(1);
								if (document.getElementById('ram')) document.getElementById('ram').innerText = data.ramUsage.toFixed(1);
								if (document.getElementById('gpu')) document.getElementById('gpu').innerText = data.gpuUsage.toFixed(1);
								if (document.getElementById('gputemp')) document.getElementById('gputemp').innerText = data.gpuTemp.toFixed(1);
							})
							.catch(err => console.error('Fetch Error:', err));
					}

					setInterval(updateStats, 1000);
					updateStats();
				</script>
			</body>
			</html>
		)rawhtml");
		page.set_header("Content-Type", "text/html"); // set the content type header to text/html
		return page;
			});

	app.port(18080).multithreaded().run(); // run the Crow application on port 18080 with multithreading enabled
}

int main() {
	std::thread serverThread(StartServer); // start the web server in a separate thread
	serverThread.detach(); // detach the server thread so it runs independently

	Sleep(500);

	PlayRandomVoiceLine(); // play random voiceline during startup

	webview::webview w(false, nullptr); // create a webview window (not resizable, no parent window)
	w.set_title("System Hardware Monitor");
	w.set_size(1600, 900, WEBVIEW_HINT_NONE);
	w.navigate("http://127.0.0.1:18080");

	HWND hwnd = FindWindowA(nullptr, "System Hardware Monitor"); // find the window handle of the webview window by its title
	if (hwnd) {
		HICON hIcon = (HICON)LoadImageA(
			GetModuleHandle(NULL), // load the icon from the resources
			MAKEINTRESOURCEA(IDI_APP_ICON), // the resource ID of the icon
			IMAGE_ICON, // specify that we are loading an icon
			GetSystemMetrics(SM_CXICON), // get the width of the icon
			GetSystemMetrics(SM_CYICON), // get the height of the icon
			LR_DEFAULTCOLOR // use the default color format for the icon
		);
		if (hIcon) {
			SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon); // set the big icon for the window
			SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon); // set the small icon for the window
		}
	}

	w.run();

	return 0;
}