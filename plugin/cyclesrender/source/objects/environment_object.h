// SPDX-License-Identifier: Apache-2.0
// The "Cycles Environment" object: the world Blender renders around the scene, as a colour, Blender's
// Sky Texture or an HDRI.

#ifndef CYCLESRENDER_ENVIRONMENT_OBJECT_H__
#define CYCLESRENDER_ENVIRONMENT_OBJECT_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object holds a type
/// (Color, Sky Texture, HDRI), a strength and each type's settings (res/description/ocyclesenvironment.h);
/// the Sky Texture's and the HDRI's are Blender's own, named, offered and defaulted as the node table
/// has them (scene/environment_export.h). It draws three circles and its axes in the viewport, and for
/// the Sky Texture a line towards the sun. It has the green "enabled" check in the Object Manager.
/// The exporter writes it as the `world` (scene/environment_export.h, ExportEnvironmentWorld), when it
/// is the highest environment in the Object Manager that renders (scene/scene_exporter.h).
cinema::Bool RegisterEnvironmentObject();

} // namespace cyclesrender

#endif // CYCLESRENDER_ENVIRONMENT_OBJECT_H__
