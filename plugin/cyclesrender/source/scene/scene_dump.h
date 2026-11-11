// SPDX-License-Identifier: Apache-2.0
// Development aid: list what the exporter would send for the active document.

#ifndef CYCLESRENDER_SCENE_DUMP_H__
#define CYCLESRENDER_SCENE_DUMP_H__

#include "c4d.h"

namespace cyclesrender
{

/// True when Cinema 4D was started with g_cyclesRenderDumpFile=<file>.
maxon::Bool IsSceneDumpRequested();

//----------------------------------------------------------------------------------------
/// Writes SceneExporter::GetListing() for the active document into the file named by
/// g_cyclesRenderDumpFile, replacing its content. No Blender is involved. The document must
/// have been evaluated (ExecutePasses) by the caller.
/// Triggered from tests with GePluginMessage(<renderer id>, ...); see tests/c4d/traversal.py.
//----------------------------------------------------------------------------------------
maxon::Bool DumpActiveDocument();

} // namespace cyclesrender

#endif // CYCLESRENDER_SCENE_DUMP_H__
