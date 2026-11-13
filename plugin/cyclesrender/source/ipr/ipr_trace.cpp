// SPDX-License-Identifier: Apache-2.0
// The IPR's timeline for measurements (ipr_trace.h).

#include "c4d.h"
#include "maxon/atomictypes.h"
#include "maxon/spinlock.h"

#include "../platform/shared_memory.h"
#include "ipr_trace.h"

namespace cyclesrender
{

namespace
{
struct TraceEvent
{
	maxon::Float64 time = 0.0;
	const maxon::Char* what = nullptr;
	maxon::Float64 value = 0.0;
};
}

static const maxon::Int TRACE_LIMIT = 16384;

static maxon::AtomicBool g_tracing;
static maxon::Spinlock g_traceLock;
static maxon::BaseArray<TraceEvent> g_trace;

void TraceIpr(const maxon::Char* what, maxon::Float64 value)
{
	if (!g_tracing.LoadRelaxed())
		return;
	TraceEvent event;
	event.time = platform::UnixTime();
	event.what = what;
	event.value = value;
	maxon::ScopedLock guard(g_traceLock);
	if (g_trace.GetCount() >= TRACE_LIMIT)
		g_trace.Erase(0, TRACE_LIMIT / 4) iferr_ignore("Erasing the oldest events of a full array cannot fail.");
	g_trace.Append(event) iferr_ignore("A lost event only shortens the timeline.");
}

void SetIprTracing(maxon::Bool on)
{
	maxon::ScopedLock guard(g_traceLock);
	g_trace.Reset();
	g_tracing.Set(on);
}

maxon::Bool IsIprTracing()
{
	return g_tracing.LoadRelaxed();
}

maxon::String TakeIprTrace()
{
	maxon::BaseArray<TraceEvent> events;
	{
		maxon::ScopedLock guard(g_traceLock);
		events = std::move(g_trace);
		g_trace.Reset();
	}
	maxon::String text = "["_s;
	for (maxon::Int i = 0; i < events.GetCount(); ++i)
	{
		const TraceEvent& event = events[i];
		if (i > 0)
			text += ","_s;
		text += FormatString("[@,\"@\",@]", maxon::String::FloatToString(event.time, -1, 6), maxon::String(event.what), maxon::String::FloatToString(event.value, -1, 3));
	}
	return text + "]"_s;
}

} // namespace cyclesrender
