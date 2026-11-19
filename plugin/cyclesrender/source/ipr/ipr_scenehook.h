// SPDX-License-Identifier: Apache-2.0
// The IPR in Cinema 4D's modal loops: a scene hook that tells the IPR the active document was
// evaluated, and the frame thread's way to the window, both for the time the window gets neither a
// timer nor a message.
//
// Measured (2026-11-16; docs/reference/c4d-sdk.md, "Modal loops and the IPR window"): while a slider
// of the Attribute Manager is dragged, Cinema 4D sets the value and evaluates and draws the document
// at every step, on the main thread or a drawing thread, but no core message reaches a dialog and its
// timer does not run until the mouse is released. What takes part in the evaluation is called then
// (a scene hook is, for every document and every kind of light), and jobs handed to the main thread's
// queue run inside the loop.
//
// With the Preferences' live updates off both do nothing (IsIprInModalLoop is false).
//
// The document's caches are read at that point too: the meshes of generators and
// deformed objects that changed. They are built when the hook runs (measured, with a copy instance placed
// before its source: at "Generators + 1" every cache was the step's), and they must not be built anew
// while they are read. Only the thread that evaluates can promise that, so an evaluation on a thread
// other than the main thread is followed on that thread, inside the hook
// (IprController::FollowEvaluation; the controller's lock keeps the main thread out meanwhile). Handing
// it to the main thread instead would not do: measured in the Attribute Manager's drag, the main thread
// takes a job from its queue only when the next mouse message arrives (a job waited 15 to 19 ms at the
// median with mouse messages 25 ms apart, and a message posted to the thread did not hurry it), and
// that message also starts the next evaluation, which builds the caches anew. So a call that reaches
// the window through the queue (a frame's, and the one after each evaluation that lets the window show
// the newest frame) reads no cache.
//
// What the evaluating thread may do there is narrow, and written down as the controller's contract
// (the class comment of IprController): it reads objects of the evaluated document only, and asks
// neither for the active document nor for a view or its camera, which are the main thread's. The call
// through the queue is also what brings those: the camera of a view, or one chosen since.

#ifndef CYCLESRENDER_IPR_SCENEHOOK_H__
#define CYCLESRENDER_IPR_SCENEHOOK_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the scene hook (ids.h, ID_IPR_SCENE_HOOK). Called once from PluginStart().
cinema::Bool RegisterIprSceneHook();

/// Any thread, for the frame thread (FrameWatcher::SetArrivalCallback): a frame arrived. In a
/// modal loop the window is asked to show it (FollowIprEvaluation, which then reads no cache of the
/// document) from the main thread's queue; otherwise nothing (the core message does it).
void NoteIprFrameArrived();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_SCENEHOOK_H__
