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
/// with a radius per point, and a material for them.
///
/// The shape is the one Cinema 4D renders: the hairs are generated with the hair material applied,
/// so its bend, curl, frizz, kink, twist, clump, length and scale are in the points (since 0.3.0;
/// before, the hairs were generated without the material and followed the guides only). Thickness at
/// root and tip comes from the hair material too.
///
/// The colour: with @p ownMaterialId empty, a Principled Hair BSDF coloured from root to tip by the
/// hair material's colour gradient, all of its knots (the hair material's specular, transparency and
/// other shading channels are not translated; the console says so once for each Hair object with a
/// hair material). With @p ownMaterialId, that material is used instead and none is written: the
/// material of a texture tag on the Hair object, which then gives the colour and the shading while
/// the hair material still gives the shape and the thickness.
///
/// Generating the hairs is what a Hair object costs (measured, 50,000 hairs of 12 segments: 13 ms
/// without the material, 87 to 376 ms with it). With @p reuse the `curves` item is left as Blender
/// holds it when nothing it is made from has changed since it was noted (@p note): the Hair object and
/// its settings, guides and tags, the hair material, the object it grows on, their places and the document's unit.
///
/// @param[in] op      A Hair object (Ohair). It is locked while its hairs are generated.
/// @param[in] reuse   What Blender holds (the IPR), or nullptr: the hairs are generated.
/// @param[in] note    Notes what the item was made from, for a later @p reuse.
/// @param[out] kept   Set when the item was left as Blender holds it. May be nullptr.
/// @return            False when the object has no hairs and nothing was written.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportHair(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, cinema::BaseObject* op,
	const maxon::String& id, const maxon::String& ownMaterialId = maxon::String(), const SyncState* reuse = nullptr, maxon::Bool note = false,
	maxon::Bool* kept = nullptr);

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
