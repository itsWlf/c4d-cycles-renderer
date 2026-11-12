// SPDX-License-Identifier: Apache-2.0
// A file that stays undeletable while this process runs: the sign, for other processes, that
// a folder is still in use (materials/baked_files.h).
// Plain C++ types only, like the other platform headers.

#ifndef CYCLESRENDER_PLATFORM_FILE_LOCK_H__
#define CYCLESRENDER_PLATFORM_FILE_LOCK_H__

namespace cyclesrender
{
namespace platform
{

/// The id of this process.
long long CurrentProcessId();

/// Opens the file at @p utf8Path and keeps it open so that no other process can delete it, or
/// hold it in its turn, until ReleaseFileLock() or the end of this process, a crash included:
/// then the system closes it, and the file can be deleted again.
/// @param[in] create  True to create the file when it does not exist; false to fail then.
/// @return            nullptr when the file could not be opened, or when another process holds it.
void* HoldFileLock(const char* utf8Path, bool create);

/// Closes a file opened by HoldFileLock(); the file itself stays. nullptr is ignored.
void ReleaseFileLock(void* lock);

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_FILE_LOCK_H__
