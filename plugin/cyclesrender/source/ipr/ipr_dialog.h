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
///   open, zoom <percent>, fit, snapshot, save <file>, pick_camera <position in the dropdown's list of cameras>,
///   click start|pause|reload|lock|snapshot|frame|settings|close|restart_blender|show_blender|scale_50|scale_100
/// @param[out] problem  Why the command could not be carried out.
/// @return              False when @p command is not a window command.
//----------------------------------------------------------------------------------------
maxon::Bool RunIprDialogCommand(const maxon::String& command, const maxon::String& argument, maxon::String& problem);

/// Number of scene cameras the window's dropdown lists; NOTOK when the window is closed. For tests.
maxon::Int GetIprDialogCameraCount();

/// The frame the open window shows, or nullptr when the window is closed.
const Frame* GetIprDialogFrame();

/// Zoom of the window's picture (1 = 100 %) and the time the last display conversion took, for tests.
void GetIprDialogInfo(maxon::Float64& zoom, maxon::Float64& convertMilliseconds, maxon::Bool& open);

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_DIALOG_H__
