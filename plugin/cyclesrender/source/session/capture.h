// SPDX-License-Identifier: Apache-2.0
// Development aid: a text file recording what the plugin exchanges with the bridge.

#ifndef CYCLESRENDER_CAPTURE_H__
#define CYCLESRENDER_CAPTURE_H__

#include "c4d.h"

namespace cyclesrender
{

/// True when Cinema 4D was started with g_cyclesRenderCapture=<file>.
maxon::Bool IsCapturing();

//----------------------------------------------------------------------------------------
/// Appends one line "<kind> <text>" to the capture file; does nothing when not capturing.
/// Kinds written by the plugin:
///   request   the JSON header of a request, followed by " blobs=[<byte lengths>]"
///   reply     the JSON header of the bridge's reply
///   scene     one line of SceneExporter::GetListing() per exported item, before each scene request
/// Tests read the file to check what was sent without a Blender-side probe.
//----------------------------------------------------------------------------------------
void Capture(const maxon::Char* kind, const maxon::String& text);

} // namespace cyclesrender

#endif // CYCLESRENDER_CAPTURE_H__
