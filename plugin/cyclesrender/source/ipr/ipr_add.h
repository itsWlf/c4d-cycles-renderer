// SPDX-License-Identifier: Apache-2.0
// What the IPR window's menus "Objects" and "Materials" add to the active document: what the plugin
// offers, made right there, each in one undo step. Also the command "Cycles Camera", which adds the
// camera with its tag from the Extensions menu and the Commander.

#ifndef CYCLESRENDER_IPR_ADD_H__
#define CYCLESRENDER_IPR_ADD_H__

#include "c4d.h"

namespace cyclesrender
{

/// What can be added. The IPR window's menus list the objects and the materials; the three tags on
/// the selection are in no menu since F11 (0.3.0) and stay for the test hook's commands.
enum class IPRADD
{
	CYCLES_MATERIAL,				///< a material of Cycles nodes, as the command "Cycles Material" makes it
	CYCLES_WORLD_MATERIAL,	///< a material of Cycles nodes set up as a world, as the command "Cycles World Material" makes it
	CAMERA_WITH_TAG,				///< a camera where the active view looks from, with a Cycles Camera tag, made the render view's camera
	CAMERA_TAG,							///< a Cycles Camera tag on every selected camera that has none
	OBJECT_TAG,							///< a Cycles Object tag on every selected object that has none
	CYCLES_LIGHT,						///< a Cycles Light where Cinema 4D's own Create menu puts one (its command: at the workplane)
	LIGHT_TAG,							///< a Cycles Light tag on every selected light of Cinema 4D or Redshift that has none
	CYCLES_FOG,							///< a Cycles Fog, as the Cycles Light is added (its command: at the workplane)
	CYCLES_ENVIRONMENT,			///< a Cycles Environment, as the Cycles Light is added
	CYCLES_SCATTER,					///< a Cycles Scatter, as the Cycles Light is added (its children and surface are the user's to give)
	CYCLES_PROXY						///< a Cycles Proxy, as the Cycles Light is added (its file is the user's to choose)
} MAXON_ENUM_LIST(IPRADD);

//----------------------------------------------------------------------------------------
/// Adds @p what to @p doc in one undo step and makes it the active object, tag or material.
/// @param[out] message  What was done or why nothing was, in words for the user (the status bar).
/// @return              The number of things added.
//----------------------------------------------------------------------------------------
maxon::Int AddToDocument(cinema::BaseDocument* doc, IPRADD what, maxon::String& message);

//----------------------------------------------------------------------------------------
/// Registers the command "Cycles Camera" (ID_CAMERA_COMMAND, the Cycles Camera tag's icon): adds
/// IPRADD::CAMERA_WITH_TAG to the active document and shows what was done in the status bar.
/// @return  Whether Cinema 4D took the command.
//----------------------------------------------------------------------------------------
cinema::Bool RegisterCameraCommand();

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
