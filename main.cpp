#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <sysinfoapi.h>

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

int main() {
	crow::SimpleApp app;

	CROW_ROUTE(app, "/api/stats")
	([]() {
		crow::json::wvalue res;
		res["appName"] = "System Monitor Program";
		res["ramUsage"] = GetRamUsage();
		res["status"] = "Running";
		return res;
	});
	
	app.port(18080).multithreaded().run();
}