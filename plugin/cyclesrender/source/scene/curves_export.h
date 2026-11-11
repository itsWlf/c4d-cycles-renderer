// SPDX-License-Identifier: Apache-2.0
// Writes a Hair object as a `curves` item, and a particle group as a `pointcloud` item.

#ifndef CYCLESRENDER_CURVES_EXPORT_H__
#define CYCLESRENDER_CURVES_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "scene_writer.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Exports the hairs of a Hair object (the rendered hairs, not only its guides) as curves
/// with a radius per point, and a material for them: a Principled Hair BSDF coloured from
/// root to tip by the hair material's colour gradient. Thickness at root and tip comes from
/// the hair material too. Every other property of the hair material (frizz, kink, clumping
/// and so on) changes the shape Cinema 4D renders and is not applied.
///
/// @param[in] op  A Hair object (Ohair). It is locked while its hairs are generated.
/// @return        False when the object has no hairs and nothing was written.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportHair(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, cinema::BaseObject* op,
	const maxon::String& id);

//----------------------------------------------------------------------------------------
/// Exports a particle group (Ofpgroup) as points with a radius and, when the group has
/// colours, a colour each. A material reads the colour with an Attribute node named
/// `particle_color`; without @p materialId a plain material doing that is written.
///
/// @param[in] mg          Global matrix of the group.
/// @param[in] materialId  The material of the group's texture tag, or empty.
/// @return                False when the group holds no particles and nothing was written.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportParticles(SceneWriter& writer, const Coords& coords, const cinema::BaseObject* op, const cinema::Matrix& mg,
	const maxon::String& id, const maxon::String& materialId);

} // namespace cyclesrender

#endif // CYCLESRENDER_CURVES_EXPORT_H__
