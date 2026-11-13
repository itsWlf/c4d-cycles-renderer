// SPDX-License-Identifier: Apache-2.0
// The "Cycles Light" tag: Blender's light settings, on one of Cinema 4D's lights.

#ifndef CYCLESRENDER_LIGHT_TAG_H__
#define CYCLESRENDER_LIGHT_TAG_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the tag with Cinema 4D. Called once from PluginStart(). Its four tabs each start
/// with a switch (res/description/tcycleslight.h): Light replaces what the light emits with
/// Blender's settings; Settings, Visibility and Eevee add Blender's settings that Cinema 4D's
/// lights do not have. The exporter applies it in scene/light_export.cpp (ExportLight).
cinema::Bool RegisterLightTag();

/// True for the lights a Cycles Light tag works on: Cinema 4D's standard light and Redshift's.
maxon::Bool IsTaggableLight(const cinema::BaseObject* op);

/// The Cycles Light tag that counts for @p light: the first one on the light itself (as for the
/// Cycles Camera tag, a tag on a parent counts for nothing). Null when the light has none.
const cinema::BaseTag* FindLightTag(const cinema::BaseObject* light);

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_TAG_H__
