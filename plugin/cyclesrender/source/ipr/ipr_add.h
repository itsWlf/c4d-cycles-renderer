// SPDX-License-Identifier: Apache-2.0
// What the IPR window's "Add" menu adds to the active document: what the plugin offers, made
// right there, each in one undo step.

#ifndef CYCLESRENDER_IPR_ADD_H__
#define CYCLESRENDER_IPR_ADD_H__

#include "c4d.h"

namespace cyclesrender
{

/// The entries of the "Add" menu.
enum class IPRADD
{
	CYCLES_MATERIAL,				///< a material of Cycles nodes, as the command "Cycles Material" makes it
	CAMERA_WITH_TAG,				///< a camera where the active view looks from, with a Cycles Camera tag, made the render view's camera
	CAMERA_TAG,							///< a Cycles Camera tag on every selected camera that has none
	OBJECT_TAG,							///< a Cycles Object tag on every selected object that has none
	CYCLES_LIGHT,						///< a Cycles Light where the render view looks from, shining where it looks
	LIGHT_TAG								///< a Cycles Light tag on every selected light of Cinema 4D or Redshift that has none
} MAXON_ENUM_LIST(IPRADD);

//----------------------------------------------------------------------------------------
/// Adds @p what to @p doc in one undo step and makes it the active object, tag or material.
/// @param[out] message  What was done or why nothing was, in words for the user (the status bar).
/// @return              The number of things added.
//----------------------------------------------------------------------------------------
maxon::Int AddToDocument(cinema::BaseDocument* doc, IPRADD what, maxon::String& message);

//----------------------------------------------------------------------------------------
/// Keeps the render view on the right camera through undo and redo of "Camera with Cycles
/// Camera Tag". Cinema 4D's undo cannot hold a view (after undoing a step that recorded one, the
/// view has lost its camera link), so that step leaves the view out and the plugin follows it
/// instead: while the added camera is gone (undone, or deleted) the view looks through the camera
/// it looked through before, and once it is back (redone) through the added camera again. A
/// switch is forgotten when its document closes or the user chooses another camera for that view.
/// Call on the main thread after a change of a document: the message plugin of the renderer's id
/// does, on every EVMSG_CHANGE and once a second (materials/baked_files.cpp).
/// @return  How many views were set.
//----------------------------------------------------------------------------------------
maxon::Int FollowAddedCameras();
/// The switches still followed; for tests.
maxon::Int GetAddedCameraSwitchCount();
/// Forgets every switch; when the plugin is unloaded.
void FreeAddedCameraSwitches();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_ADD_H__
