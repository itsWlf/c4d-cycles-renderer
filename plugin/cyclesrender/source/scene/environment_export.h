// SPDX-License-Identifier: Apache-2.0
// Writes the `world` item: what the scene sees where there is no object.

#ifndef CYCLESRENDER_ENVIRONMENT_EXPORT_H__
#define CYCLESRENDER_ENVIRONMENT_EXPORT_H__

#include "c4d.h"

#include "../materials/material_export.h"
#include "coords.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Exports the environment.
///
/// With a Sky object, its material's luminance channel (or colour channel, if luminance is
/// off) becomes the world: a bitmap there is used as a spherical panorama, turned with the
/// Sky object. Without a Sky object the world is the plain @p defaultColor at
/// @p defaultStrength from the render settings. Colours go through the document's colour
/// settings (MaterialExporter::GetColors) like any other colour parameter; a Redshift dome's
/// are sent as stored.
///
/// @param[in] sky          The Sky object, or nullptr.
/// @param[in] skyMg        Its global matrix.
/// @param[in] skyMaterial  The material of its texture tag, or nullptr.
//----------------------------------------------------------------------------------------
maxon::Result<void> ExportWorld(MaterialExporter& materials, const cinema::BaseObject* sky, const cinema::Matrix& skyMg,
	cinema::BaseMaterial* skyMaterial, const cinema::Vector& defaultColor, maxon::Float64 defaultStrength);

//----------------------------------------------------------------------------------------
/// The environment from a Redshift dome light: its map as a spherical panorama turned with the
/// light, times 2^exposure; without a map, its colour.
//----------------------------------------------------------------------------------------
maxon::Result<void> ExportDomeWorld(MaterialExporter& materials, const cinema::BaseObject* dome, const cinema::Matrix& domeMg);

} // namespace cyclesrender

#endif // CYCLESRENDER_ENVIRONMENT_EXPORT_H__
