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
///   answered, frame (value: frame id), and what a test adds. Navigating from the picture (F17):
///   input (value: BFM_INPUT_CHANNEL of a press in the picture), input_qualifier, input_hotkeys (the
///   camera keys Cinema 4D reports held: rotate + 4 x move + 16 x scale, each 1 mouse, 2 keyboard),
///   drag_dx, drag_dy, drag_qualifier (the picture's own pan), nav_begin (value: IPRNAV), nav_pick_ms,
///   nav_step, nav_moved, nav_view_ms (a redraw of Cinema 4D's view), nav_refused, nav_end, nav_cancel.
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
