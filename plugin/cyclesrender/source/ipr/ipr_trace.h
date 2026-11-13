// SPDX-License-Identifier: Apache-2.0
// A timeline of what the IPR does, for measuring how a change travels to the picture (tests and
// measurements; off unless the test hook switches it on).

#ifndef CYCLESRENDER_IPR_TRACE_H__
#define CYCLESRENDER_IPR_TRACE_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Notes an event of the IPR with the time it happened: from the main thread (the window's
/// timer and messages, the controller), the worker (a request answered) and the frame thread (a
/// frame arrived). Does nothing while tracing is off, which is the default: the test hook's
/// "trace on" switches it on (ipr_testhook.h). The names are fixed texts (no copies are made):
///   timer, tick, change, recalc, editormove (value: MOVE_START 0, CONTINUE 1, END 2), framemsg,
///   drawn, shown (value: frame id), camera_seen, posted_view, posted_scene (value: items), export_ms,
///   answered, frame (value: frame id), and what a test adds.
//----------------------------------------------------------------------------------------
void TraceIpr(const maxon::Char* what, maxon::Float64 value = 0.0);

/// Switches tracing on or off; switching it on forgets what was noted before.
void SetIprTracing(maxon::Bool on);
maxon::Bool IsIprTracing();

/// What was noted since the last call (up to the newest 16384 events), as JSON:
/// [[seconds since 1970, "name", value], ...], and forgets it.
maxon::String TakeIprTrace();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_TRACE_H__
