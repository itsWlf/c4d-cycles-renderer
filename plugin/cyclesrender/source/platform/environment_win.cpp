// SPDX-License-Identifier: Apache-2.0
// Windows implementation of platform/environment.h. Includes no Cinema 4D headers on purpose.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "environment.h"

namespace cyclesrender
{
namespace platform
{

bool GetEnvironmentText(const char* name, char* buffer, int bufferSize)
{
	if (!name || !buffer || bufferSize < 1)
		return false;
	buffer[0] = 0;

	// The wide-character calls are used so that a path with letters outside the system's
	// code page (a user name, for example) survives.
	wchar_t wideName[128];
	if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wideName, 128) == 0)
		return false;

	wchar_t wideValue[2048];
	const DWORD length = ::GetEnvironmentVariableW(wideName, wideValue, 2048);
	if (length == 0 || length >= 2048)
		return false;

	if (::WideCharToMultiByte(CP_UTF8, 0, wideValue, -1, buffer, bufferSize, nullptr, nullptr) == 0)
	{
		buffer[0] = 0;
		return false;
	}
	return true;
}

} // namespace platform
} // namespace cyclesrender

#endif // _WIN32
