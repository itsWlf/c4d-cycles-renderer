// SPDX-License-Identifier: Apache-2.0
// The "Cycles Fog" object: a box drawn as lines that renders as fog, Blender's Principled Volume
// filling it.

#ifndef CYCLESRENDER_FOG_OBJECT_H__
#define CYCLESRENDER_FOG_OBJECT_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object holds the box's
/// size and the Principled Volume's values (res/description/ocyclesfog.h, defaults in
/// scene/fog_export.h), draws the box's twelve edges in the viewport in the object's colour and has
/// three handles for its size, as Cinema 4D's Cube. It builds no cache and shows nothing in Cinema
/// 4D's own renderers. The exporter finds it in the walk and writes it as a closed box mesh with a
/// material that has a volume and no surface (scene/fog_export.h). It has the green "enabled" check in
/// the Object Manager, as the Cycles Light has; unchecked it is not exported.
cinema::Bool RegisterFogObject();

} // namespace cyclesrender

#endif // CYCLESRENDER_FOG_OBJECT_H__
