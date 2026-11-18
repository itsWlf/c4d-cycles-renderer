// SPDX-License-Identifier: Apache-2.0
// The IPR in Cinema 4D's modal loops: a scene hook that tells the IPR the active document was
// evaluated, and the frame thread's way to the window, both for the time the window gets neither a
// timer nor a message (0.2.0, F18).
//
// Measured (2026-11-16; docs/reference/c4d-sdk.md, "Modal loops and the IPR window"): while a slider
// of the Attribute Manager is dragged, Cinema 4D sets the value and evaluates and draws the document
// at every step, on the main thread or a drawing thread, but no core message reaches a dialog and its
// timer does not run until the mouse is released. What takes part in the evaluation is called then
// (a scene hook is, for every document and every kind of light), and jobs handed to the main thread's
// queue run inside the loop.
//
// With the Preferences' live updates off (0.2.0, F22) both do nothing (IsIprInModalLoop is false).

#ifndef CYCLESRENDER_IPR_SCENEHOOK_H__
#define CYCLESRENDER_IPR_SCENEHOOK_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the scene hook (ids.h, ID_IPR_SCENE_HOOK). Called once from PluginStart().
cinema::Bool RegisterIprSceneHook();

/// Any thread, for the frame thread (FrameWatcher::SetArrivalCallback): a frame arrived. In a
/// modal loop the window is asked to show it (FollowIprEvaluation) from the main thread's queue;
/// otherwise nothing (the core message does it).
void NoteIprFrameArrived();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_SCENEHOOK_H__
