// SPDX-License-Identifier: Apache-2.0
// Windows implementation of platform/file_lock.h. Includes no Cinema 4D headers on purpose.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "file_lock.h"

namespace cyclesrender
{
namespace platform
{

long long CurrentProcessId()
{
	return static_cast<long long>(::GetCurrentProcessId());
}

void* HoldFileLock(const char* utf8Path, bool create)
{
	if (!utf8Path)
		return nullptr;
	wchar_t widePath[2048];
	if (::MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, widePath, 2048) == 0)
		return nullptr;

	// Opened for writing and shared for reading only: while it is open, another process that
	// tries to delete the file or to open it for writing (this function, there) fails with a
	// sharing violation.
	const HANDLE file = ::CreateFileW(widePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return nullptr;
	return file;
}

void ReleaseFileLock(void* lock)
{
	if (lock)
		::CloseHandle(static_cast<HANDLE>(lock));
}

} // namespace platform
} // namespace cyclesrender

#endif // _WIN32
