// SPDX-License-Identifier: Apache-2.0
// Windows implementation of platform/shared_memory.h. Includes no Cinema 4D headers on purpose.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "shared_memory.h"

namespace cyclesrender
{
namespace platform
{

bool SharedMemoryOpen(const char* name, SharedMemory& memory)
{
	SharedMemoryClose(memory);

	const HANDLE mapping = ::OpenFileMappingA(FILE_MAP_READ, FALSE, name);
	if (!mapping)
		return false;

	const void* const view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (!view)
	{
		::CloseHandle(mapping);
		return false;
	}

	MEMORY_BASIC_INFORMATION info = {};
	if (::VirtualQuery(view, &info, sizeof(info)) == 0)
	{
		::UnmapViewOfFile(view);
		::CloseHandle(mapping);
		return false;
	}

	memory.handle = mapping;
	memory.data = static_cast<const unsigned char*>(view);
	memory.size = static_cast<long long>(info.RegionSize);
	return true;
}

void SharedMemoryClose(SharedMemory& memory)
{
	if (memory.data)
		::UnmapViewOfFile(memory.data);
	if (memory.handle)
		::CloseHandle(static_cast<HANDLE>(memory.handle));
	memory = SharedMemory();
}

bool SignalCreate(const char* name, Signal& signal)
{
	SignalClose(signal);
	// Auto-reset: a raised signal lets exactly one wait through.
	signal.handle = ::CreateEventA(nullptr, FALSE, FALSE, name);
	return signal.handle != nullptr;
}

void SignalSet(const Signal& signal)
{
	if (signal.handle)
		::SetEvent(static_cast<HANDLE>(signal.handle));
}

bool SignalWait(const Signal& signal, int timeoutMilliseconds)
{
	if (!signal.handle)
	{
		::Sleep(static_cast<DWORD>(timeoutMilliseconds));
		return false;
	}
	return ::WaitForSingleObject(static_cast<HANDLE>(signal.handle), static_cast<DWORD>(timeoutMilliseconds)) == WAIT_OBJECT_0;
}

void SignalClose(Signal& signal)
{
	if (signal.handle)
		::CloseHandle(static_cast<HANDLE>(signal.handle));
	signal.handle = nullptr;
}

double UnixTime()
{
	FILETIME time;
	::GetSystemTimeAsFileTime(&time);
	const unsigned long long ticks = (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	// FILETIME counts 100 ns steps since 1601-01-01; 11644473600 s lie between that and 1970.
	return static_cast<double>(ticks) / 1e7 - 11644473600.0;
}

} // namespace platform
} // namespace cyclesrender

#endif // _WIN32
