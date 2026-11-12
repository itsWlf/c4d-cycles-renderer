// SPDX-License-Identifier: Apache-2.0
// Plugin IDs of the Cycles Render module.
//
// Every hook a plugin registers with Cinema 4D needs an ID that is unique among all plugins of
// its type, and IDs are issued to a developer at developers.maxon.net: never invent one. All
// IDs below are issued. A fork that is published under another name needs IDs of its own,
// because two plugins with the same ID cannot be loaded together.
//
// What Cinema 4D allows to share an ID (both found by trying):
// - IDs are kept per plugin type, so hooks without a description (parameter page) may reuse an
//   ID that another plugin type has: the IPR command and the core message share the renderer's.
// - Two hooks that each have a description cannot share one: registered under the renderer's
//   ID, the Preferences page showed the renderer's settings.

#ifndef CYCLESRENDER_IDS_H__
#define CYCLESRENDER_IDS_H__

#include "c4d.h"

namespace cyclesrender
{

/// The renderer (VideoPostData). Stored in scene files as the render engine, and the key of
/// the plugin's own settings in Cinema 4D's preferences.
static const cinema::Int32 ID_CYCLES_VIDEOPOST = 1073816;

/// The command that opens the IPR window. A command plugin, so it can share the renderer's ID.
static const cinema::Int32 ID_IPR_COMMAND = ID_CYCLES_VIDEOPOST;

/// The core message the frame watcher thread sends to the IPR window when a frame has
/// arrived. Core messages are a namespace of their own, so the renderer's ID serves.
static const cinema::Int32 ID_CORE_MESSAGE = ID_CYCLES_VIDEOPOST;

/// The message plugin that watches Cinema 4D's document list to delete the baked textures of
/// closed documents (materials/baked_files.h). A message plugin has no description and is the
/// first of its type with this number, so the renderer's ID serves.
static const cinema::Int32 ID_BAKED_FILES_WATCH = ID_CYCLES_VIDEOPOST;

/// The Preferences page.
static const cinema::Int32 ID_PREFERENCES = 1073900;

/// The "Cycles Object" tag. Stored in scene files.
static const cinema::Int32 ID_OBJECT_TAG = 1073899;

/// The command "Cycles Material", which makes a new material of Cycles nodes. (Issued for a
/// material type of its own; Cinema 4D lets only its own material type carry a node graph, so
/// the ID serves the command: materials/cycles_material.h.)
static const cinema::Int32 ID_CYCLES_MATERIAL = 1073908;

/// The "Cycles Native Material": a material edited in Blender's own Shader Editor and kept as
/// Blender's data (materials/native_material.h). Stored in scene files. The command that
/// makes one is registered with the same number.
static const cinema::Int32 ID_NATIVE_MATERIAL = 1073916;

/// The message plugin whose timer takes in what was changed in Blender's Shader Editor
/// (materials/native_material.cpp, "Editing").
static const cinema::Int32 ID_NATIVE_MATERIAL_EDITS = 1073918;

// Issued for hooks that are planned and not built yet. Once registered, each is stored in
// scene files.
/// The "Cycles Camera" tag: depth of field and lens settings of a Blender camera, on a camera.
static const cinema::Int32 ID_CAMERA_TAG = 1073985;
/// The "Cycles Light" tag: Blender's light settings, on one of Cinema 4D's lights.
static const cinema::Int32 ID_LIGHT_TAG = 1073986;
/// The "Cycles Light" object: a light with Blender's own types and settings.
static const cinema::Int32 ID_LIGHT_OBJECT = 1073987;

} // namespace cyclesrender

#endif // CYCLESRENDER_IDS_H__
