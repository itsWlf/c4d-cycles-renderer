// SPDX-License-Identifier: Apache-2.0
// The "Cycles Light" object: a light of the plugin's own, with Blender's light types and settings.

#ifndef CYCLESRENDER_LIGHT_OBJECT_H__
#define CYCLESRENDER_LIGHT_OBJECT_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object holds Blender's
/// values (res/description/ocycleslight.h, with Blender's defaults: lights/light_settings.h) and draws
/// its type, size and cone in the viewport; it emits along its +Z axis, as Cinema 4D's lights do.
/// The exporter finds it in the walk like any light and sends its values as they are
/// (scene/light_export.cpp). It does not light Cinema 4D's own viewport or renderers.
cinema::Bool RegisterLightObject();

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_OBJECT_H__
