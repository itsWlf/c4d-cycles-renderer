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
///   With the Preferences' live updates off (F22): drag_held (a tool's step in a modal loop, not sent),
///   playback_held (a tick while the timeline plays, nothing sent).
///   B11 (0.2.0): frame_due (the frame thread announces a frame the window held back, FrameWatcher::Take),
///   same (value: frame id; a frame equal to the one on screen, not converted or drawn), priority
///   (value: Cinema 4D's class rank * 10 + the rank given to Blender, IprController::FollowPriority).
///   What the IPR costs Cinema 4D's main thread (0.2.0, B11): durations in ms, each with the time it
///   ended. The entry points: timer_ms (IprDialog::Timer), core_ms (IprDialog::CoreMessage), follow_ms
///   (IprDialog::FollowEvaluation, the scene hook's call in a modal loop). Inside them: export_ms (a
///   whole export) with collect_ms, write_ms, compare_ms and mesh_ms (and meshes_kept, meshes_exported);
///   alone_ms (an update of single items), lights_ms (the lights alone), camera_ms
///   (IprController::SyncCamera); show_ms (ShowNewFrame when it took a frame) with convert_ms
///   (IprImageArea::Convert), and draw_ms (IprImageArea::DrawMsg).
///   What was sent, and what was not (0.2.0, B11): posted_scene (a whole export; value: items),
///   posted_alone (single items; value: how many), posted_lights (the lights alone, without "Optimized
///   Live Updates"), posted_view, posted_camera; gated (a redraw after which no counter of the document
///   had moved: nothing exported), settle (the whole export once things stood still), step_whole (a
///   step that could not be sent alone), drag_step (a tool's step), drag_end (its last one).
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
