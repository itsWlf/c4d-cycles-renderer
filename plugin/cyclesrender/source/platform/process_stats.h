// SPDX-License-Identifier: Apache-2.0
// Memory figures of another process and of the machine, for the IPR's status bar, and the
// scheduling of the Blender the IPR started (its priority class; the machine's processors).
// Plain C++ types only, like the other platform headers.

#ifndef CYCLESRENDER_PLATFORM_PROCESS_STATS_H__
#define CYCLESRENDER_PLATFORM_PROCESS_STATS_H__

namespace cyclesrender
{
namespace platform
{

struct MemoryStats
{
	long long processRam = 0;		///< bytes of main memory the process uses (working set)
	long long totalRam = 0;			///< bytes of main memory installed
	long long processVram = 0;	///< bytes of dedicated graphics memory the process uses, summed over graphics cards
	long long totalVram = 0;		///< bytes of dedicated memory of the largest graphics card
};

/// Fills @p stats for the process with id @p processId. Values that cannot be read stay 0.
/// Takes a few milliseconds; call it about once a second.
void QueryMemoryStats(long long processId, MemoryStats& stats);

/// The number of logical processors of this computer, counted as Blender counts them for its
/// render threads (on Windows GetSystemInfo: the processors of this process's group). At least 1.
int GetLogicalProcessorCount();

/// Priority classes as ranks, lowest first: 0 idle, 1 below normal, 2 normal, 3 above normal,
/// 4 high, 5 realtime. -1: not known.
int GetOwnPriorityRank();
int GetProcessPriorityRank(long long processId);

/// Puts the process with id @p processId into the class of rank @p rank (as above). Only for a
/// process this program started. False when it could not be done.
bool SetProcessPriorityRank(long long processId, int rank);

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_PROCESS_STATS_H__
