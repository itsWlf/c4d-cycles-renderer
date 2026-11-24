// SPDX-License-Identifier: Apache-2.0
// The file Blender writes itself from a final frame (the Output tab's "Files Written by Blender",
// 0.3.0, F6): which renders write it, where it goes for each frame, and what the bridge is told.
//
// The file is written beside the picture and the passes the Picture Viewer gets, from the same
// render: Blender's File Output node writes a multilayer OpenEXR with the passes chosen for the
// file and Cryptomatte with its manifest (docs/architecture.md, decision 36). Its passes are
// chosen apart from the Picture Viewer's; a pass only the file asks for is switched on in Blender
// too, which leaves the picture as it is (measured: docs/reference/blender.md, "Files Blender writes").

#ifndef CYCLESRENDER_FILE_OUTPUT_H__
#define CYCLESRENDER_FILE_OUTPUT_H__

#include "c4d.h"

#include "../session/json_writer.h"

namespace cyclesrender
{

/// True when this render writes Blender's file: the switch is on, and Cinema 4D renders for the
/// Picture Viewer or a file (RENDERFLAGS::EXTERNAL), not for the editor view, the interactive
/// render region or a preview. The IPR never writes it.
maxon::Bool WritesBlenderFile(const cinema::BaseContainer& settings, cinema::RENDERFLAGS flags);

/// True when the file holds Cryptomatte, whose manifest names objects and materials by their
/// names in Blender: the scene's items are then sent with Cinema 4D's names (ExportSettings::itemNames).
maxon::Bool BlenderFileNeedsNames(const cinema::BaseContainer& settings);

/// Writes a `settings` request that switches on in Blender what only the file needs: its passes
/// and Cryptomatte with its levels. Nothing is switched off: the Picture Viewer's passes were set by
/// the request before. The top-level object is left open, as BridgeClient::Request() expects.
void WriteBlenderFileSettings(JsonWriter& request, const cinema::BaseContainer& settings);

//----------------------------------------------------------------------------------------
/// The file of the frame being rendered: the Output tab's File with Cinema 4D's tokens resolved
/// ($prj, $take, $camera and the others, as for the Output page's path), a relative path taken
/// from the document's folder, and, when the job renders more than one frame and the path has no
/// $frame, the frame number placed as the Output page's Name setting places it (Name0000.TIF and
/// the six others: four or three digits, with or without a dot). The extension is always ".exr".
/// @param[out] problem  Why there is no file (no path, a relative path in a document never
///                      saved), in words for the console; empty when a path is returned.
//----------------------------------------------------------------------------------------
maxon::String GetBlenderFilePath(const cinema::BaseContainer& settings, const cinema::VideoPostStruct* vps, maxon::String& problem);

/// Writes the `render` request's `file` field for @p path: the colour depth, the codec, and the
/// passes in Blender's names ("Combined", the passes, "CryptoObject", "CryptoMaterial").
void WriteBlenderFileField(JsonWriter& request, const cinema::BaseContainer& settings, const maxon::String& path);

} // namespace cyclesrender

#endif // CYCLESRENDER_FILE_OUTPUT_H__
