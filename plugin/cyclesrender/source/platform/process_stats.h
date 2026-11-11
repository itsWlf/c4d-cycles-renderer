// SPDX-License-Identifier: Apache-2.0
// Memory figures of another process and of the machine, for the IPR's status bar.
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

} // namespace platform
} // namespace cyclesrender

#endif // CYCLESRENDER_PLATFORM_PROCESS_STATS_H__
