// SPDX-License-Identifier: Apache-2.0
// Writes a Cinema 4D standard light as a `light` item.

#ifndef CYCLESRENDER_LIGHT_EXPORT_H__
#define CYCLESRENDER_LIGHT_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "document_colors.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The scene items a light is limited to (its "Project" tab: include or exclude list).
struct LightLinking
{
	maxon::Bool include = false;                 ///< true: only these are lit; false: all but these
	maxon::BaseArray<maxon::String> itemIds;     ///< ids of `object` and `instancer` items
};

//----------------------------------------------------------------------------------------
/// Exports the light @p light, whose global matrix is @p mg, under the id @p id.
///
/// Cinema 4D's standard lights have no physical unit: at 100 % brightness a white diffuse
/// surface facing the light renders as 1.0, at any distance unless a falloff is set. Blender
/// lights always fall off with the square of the distance, so a light is given the power
/// that reproduces Cinema 4D's brightness at one reference distance: the light's
/// "Radius/Decay" value (500 cm by default), whether its falloff is switched on or not.
/// With "Inverse Square" falloff the two then agree at every distance. The constants are
/// derived in docs/reference/c4d-sdk.md, section "Light conversion".
///
/// A Redshift light (Orslight) is physical and converts exactly: infinite, point, spot and
/// area lights in the unit "Image" (light_export.cpp, ExportRedshiftLight).
///
/// @param[in] colors      The document's colour settings: a standard light's colour is a colour
///                        parameter (DocumentColors::Input). A Redshift light's is sent as stored.
/// @param[in] multiplier  The "Light Multiplier" render setting, applied to every light.
/// @return  False when the light emits nothing and was skipped.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking = nullptr);

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_EXPORT_H__
