#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <sysinfoapi.h>
#include <processthreadsapi.h>

#include <iostream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#define ASIO_STANDALONE
#define CROW_MAIN
#include "crow.h"

// Capture Ram usage
double GetRamUsage() {
	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	GlobalMemoryStatusEx(&memInfo);

	DWORDLONG totalPhysMem = memInfo.ullTotalPhys;
	DWORDLONG physMemUsed = memInfo.ullTotalPhys - memInfo.ullAvailPhys;

	return ((double)physMemUsed / (double)totalPhysMem * 100.0);
}

// Capture CPU usage
class CpuUsage {
private:
	ULARGE_INTEGER lastIdleTime;
	ULARGE_INTEGER lastKernelTime;
	ULARGE_INTEGER lastUserTime;

	void filetime_to_ularge(const FILETIME& ft, ULARGE_INTEGER& ul){
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
		// CPU usage calculation
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
		if (totalSys == 0) {
			return 0.0;
		}

		double cpuUsage = (1.0 - ((double)sysIdleDiff / (double)totalSys)) * 100.0;
		return cpuUsage < 0.0 ? 0.0 : cpuUsage > 100.0 ? 100.0 : cpuUsage;
	}
};

CpuUsage cpuUsage; // Global instance to track CPU usage over time

int main() {
	crow::SimpleApp app;

    CROW_ROUTE(app, "/assets/mascot.png")
    ([]() {
		std::string path = "assets/mascot.png";
		std::ifstream file(path, std::ios::binary);

		if (!file.is_open()) {
			path = "System Monitor App/assets/mascot.png";
			file.open(path, std::ios::binary);
		}

		if (!file.is_open()) {
			return crow::response(404, "File not found");
		}

        std::ostringstream ss;
		ss << file.rdbuf();

		crow::response res(ss.str());
		res.set_header("Content-Type", "image/png");
        return res;
    });

    CROW_ROUTE(app, "/api/stats")
	([]() {
		crow::json::wvalue res;
		res["appName"] = "System Monitor Program";
		res["ramUsage"] = GetRamUsage();
		res["cpuUsage"] = cpuUsage.GetCpuUsage();
		res["gpuUsage"] = 0.0; // Placeholder for GPU usage
		res["gpuTemp"] = 0.0; // Placeholder for GPU temperature
		res["status"] = "Running";
		return res;
	});

	CROW_ROUTE(app, "/")
		([]() {
        auto page = crow::response(R"(
            <!DOCTYPE html>
            <html lang="en">
            <head>
                <meta charset="UTF-8">
                <meta name="viewport" content="width=device-width, initial-scale=1.0">
                <title>System Hardware Monitor</title>
                <style>
                    * { box-sizing: border-box; margin: 0; padding: 0; }
                    
                    body {
                        font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, Roboto, sans-serif;
                        background: #1e1828;
                        color: #f1f5f9;
                        height: 100vh;
                        display: flex;
                        overflow: hidden;
                    }

                    .left-panel {
                        flex: 1;
                        padding: 60px;
                        display: flex;
                        flex-direction: column;
                        justify-content: center;
                        z-index: 2;
                        background: linear-gradient(90deg, #1e1828 70%, rgba(30, 24, 40, 0.2) 100%);
                    }

                    .header {
                        margin-bottom: 40px;
                        border-bottom: 2px solid rgba(249, 115, 22, 0.3);
                        padding-bottom: 15px;
                    }
                    .header h1 { font-size: 2.3rem; color: #ff9d66; font-weight: 700; letter-spacing: 0.5px; }
                    .header p { font-size: 0.95rem; color: #a099b2; margin-top: 6px; }

                    .grid-container {
                        display: grid;
                        grid-template-columns: repeat(2, 1fr);
                        gap: 22px;
                        max-width: 520px;
                    }

                    .card {
                        background: rgba(35, 28, 48, 0.7);
                        border: 1px solid rgba(255, 157, 102, 0.18);
                        backdrop-filter: blur(16px);
                        border-radius: 14px;
                        padding: 24px;
                        box-shadow: 0 10px 30px rgba(0, 0, 0, 0.35);
                        transition: all 0.25s ease;
                    }
                    .card:hover { 
                        transform: translateY(-4px); 
                        border-color: rgba(255, 157, 102, 0.5);
                        box-shadow: 0 12px 35px rgba(249, 115, 22, 0.15);
                    }

                    .card h3 { font-size: 0.85rem; color: #a099b2; text-transform: uppercase; letter-spacing: 1.2px; margin-bottom: 12px; }
                    .card .value { font-size: 2.2rem; font-weight: 700; color: #ffab76; }
                    .card .unit { font-size: 1.1rem; color: #8e83a3; font-weight: 400; }
                    .card.temp .value { color: #f97316; }

                    .right-panel {
                        width: 50%;
                        height: 100vh;
                        position: relative;
                        overflow: hidden;
                    }

                    .mascot-img {
                        width: 100%;
                        height: 100%;
                        object-fit: cover;
                        object-position: center right;
                        
                        -webkit-mask-image: linear-gradient(to right, transparent 0%, black 25%);
                        mask-image: linear-gradient(to right, transparent 0%, black 25%);
                    }
                </style>
            </head>
            <body>
                <div class="left-panel">
                    <div class="header">
                        <h1>System Monitor</h1>
                        <p>Live Hardware Performance Dashboard</p>
                    </div>

                    <div class="grid-container">
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

                        <div class="card temp">
                            <h3>GPU Temp</h3>
                            <div class="value"><span id="gputemp">--</span> <span class="unit">°C</span></div>
                        </div>
                    </div>
                </div>

                <div class="right-panel">
                    <img class="mascot-img" src="/assets/mascot.png" alt="Surtr">
                </div>

                <script>
                    function updateStats() {
                        fetch('/api/stats')
                            .then(res => res.json())
                            .then(data => {
                                document.getElementById('cpu').innerText = data.cpuUsage.toFixed(1);
                                document.getElementById('ram').innerText = data.ramUsage.toFixed(1);
                                document.getElementById('gpu').innerText = data.gpuUsage.toFixed(1);
                                document.getElementById('gputemp').innerText = data.gpuTemp.toFixed(1);
                            })
                            .catch(err => console.error('Fetch Error:', err));
                    }
                    setInterval(updateStats, 1000);
                    updateStats();
                </script>
            </body>
            </html>
        )");
        page.set_header("Content-Type", "text/html");
        return page;
        });
	
	app.port(18080).multithreaded().run();
	return 0;
}