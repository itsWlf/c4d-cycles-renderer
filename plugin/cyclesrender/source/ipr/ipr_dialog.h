// SPDX-License-Identifier: Apache-2.0
// The IPR window and the command that opens it. Its layout is described in
// docs/architecture.md, "The IPR window".

#ifndef CYCLESRENDER_IPR_DIALOG_H__
#define CYCLESRENDER_IPR_DIALOG_H__

#include "c4d.h"

#include "../session/frame_reader.h"

namespace cyclesrender
{

/// Registers the "Cycles Render IPR" command. Called once from PluginStart().
cinema::Bool RegisterIprCommand();

/// Releases the window and its icons. Called when Cinema 4D shuts down.
void FreeIprDialog();

/// Opens the IPR window, or brings it to the front.
void OpenIprDialog();

//----------------------------------------------------------------------------------------
/// For tests (ipr_testhook): what the window's own controls do.
///   open, zoom <percent>, type_zoom <percent> (as if typed into the zoom field), fit, snapshot, save <file>,
///   pick_camera <position in the dropdown's list of cameras>,
///   click start|pause|reload|lock|snapshot|frame|settings|close|restart_blender|show_blender|scale_50|scale_100,
///   click image (a plain click on the picture: starts again after a failure, or opens the Preferences
///   when no Blender can be started, IprController::IsBlenderMissing())
/// @param[out] problem  Why the command could not be carried out.
/// @return              False when @p command is not a window command.
//----------------------------------------------------------------------------------------
maxon::Bool RunIprDialogCommand(const maxon::String& command, const maxon::String& argument, maxon::String& problem);

/// Any thread: the window is open, the IPR renders, and its timer has not run for three periods,
/// so Cinema 4D is in a modal loop (a drag in a view or in the Attribute Manager). For ipr_scenehook.h.
maxon::Bool IsIprInModalLoop();

//----------------------------------------------------------------------------------------
/// Main thread: the active document has been evaluated, or a frame arrived (ipr_scenehook.h). If
/// Cinema 4D is in a modal loop, the lights are followed (IprController::FollowLights) and the
/// newest frame is shown. Otherwise nothing: the change event and the timer do that.
//----------------------------------------------------------------------------------------
void FollowIprEvaluation();

/// Number of scene cameras the window's dropdown lists; NOTOK when the window is closed. For tests.
maxon::Int GetIprDialogCameraCount();

/// The frame the open window shows, or nullptr when the window is closed.
const Frame* GetIprDialogFrame();

/// Zoom of the window's picture and the value its zoom field holds (1 = 100 %; the field 0 when
/// the window is closed), and the time the last display conversion took, for tests.
void GetIprDialogInfo(maxon::Float64& zoom, maxon::Float64& zoomField, maxon::Float64& convertMilliseconds, maxon::Bool& open);

/// The text the window's picture shows ("Press Start ...", why the IPR failed); empty when there is
/// none or the window is closed. For tests.
maxon::String GetIprDialogMessage();

/// Where the window's picture is on the screen, in screen pixels, and the image point shown in its
/// middle (render pixels). False while the window is closed. For tests that post mouse messages to it.
maxon::Bool GetIprImageScreenRect(maxon::Int32& left, maxon::Int32& top, maxon::Int32& width, maxon::Int32& height, maxon::Float64& centerX, maxon::Float64& centerY);

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_DIALOG_H__
