// SPDX-License-Identifier: Apache-2.0
// Windows implementation of platform/process_stats.h. Includes no Cinema 4D headers on purpose.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#include "process_stats.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

namespace cyclesrender
{
namespace platform
{

// Dedicated memory of the largest graphics card. It does not change while the program runs.
static long long TotalVideoMemory()
{
	static long long cached = -1;
	if (cached >= 0)
		return cached;
	cached = 0;

	IDXGIFactory1* factory = nullptr;
	if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
		return cached;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
	{
		DXGI_ADAPTER_DESC1 description = {};
		if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
		{
			const long long memory = static_cast<long long>(description.DedicatedVideoMemory);
			if (memory > cached)
				cached = memory;
		}
		adapter->Release();
	}
	factory->Release();
	return cached;
}

// Windows publishes the graphics memory of each process as performance counters named
// "pid_<id>_luid_..._phys_<n>", one per graphics card the process uses.
static long long ProcessVideoMemory(long long processId)
{
	static PDH_HQUERY query = nullptr;
	static PDH_HCOUNTER counter = nullptr;
	static long long queryProcess = 0;

	if (query && queryProcess != processId)
	{
		::PdhCloseQuery(query);
		query = nullptr;
		counter = nullptr;
	}
	if (!query)
	{
		if (::PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS)
		{
			query = nullptr;
			return 0;
		}
		wchar_t path[128];
		swprintf(path, 128, L"\\GPU Process Memory(pid_%lld_*)\\Dedicated Usage", processId);
		if (::PdhAddEnglishCounterW(query, path, 0, &counter) != ERROR_SUCCESS)
		{
			::PdhCloseQuery(query);
			query = nullptr;
			counter = nullptr;
			return 0;
		}
		queryProcess = processId;
	}

	if (::PdhCollectQueryData(query) != ERROR_SUCCESS)
		return 0;

	DWORD bufferSize = 0;
	DWORD itemCount = 0;
	if (::PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bufferSize, &itemCount, nullptr) != PDH_MORE_DATA || bufferSize == 0)
		return 0;
	PDH_FMT_COUNTERVALUE_ITEM_W* const items = static_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(malloc(bufferSize));
	if (!items)
		return 0;
	long long total = 0;
	if (::PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE, &bufferSize, &itemCount, items) == ERROR_SUCCESS)
	{
		for (DWORD i = 0; i < itemCount; ++i)
			total += items[i].FmtValue.largeValue;
	}
	free(items);
	return total;
}

void QueryMemoryStats(long long processId, MemoryStats& stats)
{
	stats = MemoryStats();

	MEMORYSTATUSEX memory = {};
	memory.dwLength = sizeof(memory);
	if (::GlobalMemoryStatusEx(&memory))
		stats.totalRam = static_cast<long long>(memory.ullTotalPhys);
	stats.totalVram = TotalVideoMemory();

	if (processId <= 0)
		return;
	const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
	if (process)
	{
		PROCESS_MEMORY_COUNTERS counters = {};
		if (::GetProcessMemoryInfo(process, &counters, sizeof(counters)))
			stats.processRam = static_cast<long long>(counters.WorkingSetSize);
		::CloseHandle(process);
	}
	stats.processVram = ProcessVideoMemory(processId);
}

} // namespace platform
} // namespace cyclesrender

#endif // _WIN32
