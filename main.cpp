#define _CRT_SECURE_NO_WARNINGS
#pragma comment(linker, "/subsystem:windows /ENTRY:mainCRTStartup")

#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "resource.h"
#include <thread>
#include <sysinfoapi.h>
#include <processthreadsapi.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define ASIO_STANDALONE
#define CROW_MAIN
#include "crow.h"

#define WEBVIEW_WINAPI
#include "webview.h"

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

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

typedef int (*nvmlInit_t)();
typedef int (*nvmlShutdown_t)();
typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int index, void** device);
typedef int (*nvmlDeviceGetTemperature_t)(void* device, int sensorType, unsigned int* temp);
typedef int (*nvmlDeviceGetUtilizationRates_t)(void* device, nvmlUtilization_t* rates);

void GetGpuStats(double& usageOut, double& tempOut) {
    usageOut = 0.0;
    tempOut = 0.0;

    HMODULE hNvml = LoadLibraryA("nvml.dll");
    if (!hNvml) return;

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
        }
        if (nvmlShutdown) nvmlShutdown();
    }

    FreeLibrary(hNvml);
}

std::string GetRealCpuName() {
    HKEY hKey;
    char cpuName[256] = "Unknown CPU";
    DWORD bufSize = sizeof(cpuName);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &bufSize);
        RegCloseKey(hKey);
    }
    return std::string(cpuName);
}

std::string GetRealTotalRam() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    double totalGB = (double)memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f GB", totalGB);
    return std::string(buf);
}

std::string GetRealGpuName() {
    DISPLAY_DEVICEA dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            return std::string(dd.DeviceString);
        }
    }
    if (EnumDisplayDevicesA(NULL, 0, &dd, 0)) {
        return std::string(dd.DeviceString);
    }
    return "Unknown GPU";
}

double GetRamUsage() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
    DWORDLONG physMemUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
    return ((double)physMemUsed / (double)totalPhysMem * 100.0);
}

class CpuUsage {
private:
    ULARGE_INTEGER lastIdleTime, lastKernelTime, lastUserTime;
    void filetime_to_ularge(const FILETIME& ft, ULARGE_INTEGER& ul) {
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
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
        filetime_to_ularge(idleTime, idle);
        filetime_to_ularge(kernelTime, kernel);
        filetime_to_ularge(userTime, user);

        ULONGLONG sysIdleDiff = idle.QuadPart - lastIdleTime.QuadPart;
        ULONGLONG sysKernelDiff = kernel.QuadPart - lastKernelTime.QuadPart;
        ULONGLONG sysUserDiff = user.QuadPart - lastUserTime.QuadPart;

        lastIdleTime = idle;
        lastKernelTime = kernel;
        lastUserTime = user;

        ULONGLONG totalSys = sysKernelDiff + sysUserDiff;
        if (totalSys == 0) return 0.0;

        double cpuUsage = (1.0 - ((double)sysIdleDiff / (double)totalSys)) * 100.0;
        return cpuUsage < 0.0 ? 0.0 : (cpuUsage > 100.0 ? 100.0 : cpuUsage);
    }
};

CpuUsage cpuUsage;

void StartServer() {
    crow::SimpleApp app;

    CROW_ROUTE(app, "/assets/mascot.jpg")
        ([]() {
        std::string imgData = GetResourceData(IDR_MASCOT1_IMG, MAKEINTRESOURCEA(10)); // RT_RCDATA = 10
        crow::response res(imgData);
        res.set_header("Content-Type", "image/jpeg");
        return res;
            });

    CROW_ROUTE(app, "/assets/mascot2.jpg")
        ([]() {
        std::string imgData = GetResourceData(IDR_MASCOT2_IMG, MAKEINTRESOURCEA(10)); // RT_RCDATA = 10
        crow::response res(imgData);
        res.set_header("Content-Type", "image/jpeg");
        return res;
            });

    CROW_ROUTE(app, "/api/stats")
        ([]() {
        double gpuUsage = 0.0, gpuTemp = 0.0;
        GetGpuStats(gpuUsage, gpuTemp);
        crow::json::wvalue res;
        res["ramUsage"] = GetRamUsage();
        res["cpuUsage"] = cpuUsage.GetCpuUsage();
        res["gpuUsage"] = gpuUsage;
        res["gpuTemp"] = gpuTemp;
        return res;
            });

    CROW_ROUTE(app, "/api/specs")
        ([]() {
        crow::json::wvalue res;
        res["cpuName"] = GetRealCpuName();
        res["totalRam"] = GetRealTotalRam();
        res["gpuName"] = GetRealGpuName();
        return res;
            });

    CROW_ROUTE(app, "/")
        ([]() {
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
                        grid.innerHTML = `
                            <div class="card full-width">
                                <h3>Processor (CPU)</h3>
                                <div class="value" style="font-size: 1.25rem;">${data.cpuName}</div>
                            </div>
                            <div class="card full-width">
                                <h3>Graphics Card (GPU)</h3>
                                <div class="value" style="font-size: 1.25rem;">${data.gpuName}</div>
                            </div>
                            <div class="card full-width">
                                <h3>Total Memory</h3>
                                <div class="value">${data.totalRam}</div>
                            </div>
                        `;
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
        page.set_header("Content-Type", "text/html");
        return page;
            });

    app.port(18080).multithreaded().run();
}

int main() {
    PlaySound(MAKEINTRESOURCE(IDR_START_WAVE), GetModuleHandle(NULL), SND_RESOURCE | SND_ASYNC);

    std::thread serverThread(StartServer);
    serverThread.detach();

    Sleep(500);

    webview::webview w(false, nullptr);
    w.set_title("System Hardware Monitor");
    w.set_size(1200, 600, WEBVIEW_HINT_NONE);
    w.navigate("http://127.0.0.1:18080");
    
    HWND hwnd = FindWindowA(nullptr, "System Hardware Monitor");
    if (hwnd) {
        HICON hIcon = (HICON)LoadImageA(
            GetModuleHandle(NULL),
            MAKEINTRESOURCEA(IDI_APP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR
        );
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }
    
    w.run();

    return 0;
}