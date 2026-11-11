// SPDX-License-Identifier: Apache-2.0
// Read access to named shared memory created by another process (the bridge in Blender),
// and the wall clock both processes share.
//
// Like socket.h, this header uses only plain C++ types: the implementation includes system
// headers that clash with Cinema 4D's.

#ifndef CYCLESRENDER_PLATFORM_SHARED_MEMORY_H__
#define CYCLESRENDER_PLATFORM_SHARED_MEMORY_H__

namespace cyclesrender
{
namespace platform
{

struct SharedMemory
{
	void* handle = nullptr;
	const unsigned char* data = nullptr;	///< first byte, or nullptr while closed
	long long size = 0;										///< bytes that may be read (rounded up to whole pages by the system)
};

/// Opens an existing block for reading. @p name is plain ASCII. Returns false when no block
/// of that name exists.
bool SharedMemoryOpen(const char* name, SharedMemory& memory);

/// Closes the block. Safe to call on a closed one.
void SharedMemoryClose(SharedMemory& memory);

/// A signal one thread or process raises and another waits for (a Windows event).
struct Signal
{
	void* handle = nullptr;
};

/// Creates a signal, or opens the existing one of that name. @p name is plain ASCII, or
/// nullptr for a signal used inside this process only.
bool SignalCreate(const char* name, Signal& signal);

/// Raises the signal: one waiting SignalWait() returns true.
void SignalSet(const Signal& signal);

/// Waits until the signal is raised. Returns false when the time ran out first.
bool SignalWait(const Signal& signal, int timeoutMilliseconds);

void SignalClose(Signal& signal);

/// Seconds since 1970-01-01 UTC, as Python's time.time() returns in the bridge.
double UnixTime();

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_SHARED_MEMORY_H__
