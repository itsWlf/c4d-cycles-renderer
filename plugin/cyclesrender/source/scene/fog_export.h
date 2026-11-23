// SPDX-License-Identifier: Apache-2.0
// Writes a Cycles Fog object (objects/fog_object.h) as a box mesh with a volume material.

#ifndef CYCLESRENDER_FOG_EXPORT_H__
#define CYCLESRENDER_FOG_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "document_colors.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The edge of a new fog box in document units, on every axis: Cinema 4D's Cube's.
static const maxon::Float64 FOG_DEFAULT_SIZE = 200.0;

/// The density a new fog starts with, per metre (Blender's own default is 1.0). Chosen by
/// measurement (docs/reference/blender.md, "The Cycles Fog object"): through the 2 m of a new box a
/// bright background keeps exp(-2 x 0.2) = 67 % of its light, which reads as fog and not as a wall;
/// Blender's 1.0 leaves 14 %, a grey block at Cinema 4D's scale of a few metres.
static const maxon::Float64 FOG_DEFAULT_DENSITY = 0.2;

/// The cells of a new fog's noise grid along the box's longest edge (Resolution).
static const maxon::Int32 FOG_DEFAULT_NOISE_RESOLUTION = 64;

/// Whether @p fog is switched on by its green check in the Object Manager (the object's deform mode,
/// as for the Cycles Light, scene/light_export.h: IsLightSwitchedOn). The check counts for the fog
/// itself only, not for the objects below it.
maxon::Bool IsFogSwitchedOn(const cinema::BaseObject* fog);

//----------------------------------------------------------------------------------------
/// Writes the box and its material of the Cycles Fog @p fog, in the fog's own axes (the `object`
/// item that shows them, with the fog's matrix, is the caller's):
///
/// - the mesh @p meshId: the closed box of the fog's size around its origin, six quads facing out,
///   and its points item @p meshId + ".points" (sent with it, as ExportMesh() does);
/// - the material @p materialId: a Principled Volume with the fog's values (colours through
///   @p colors, as a material's) on the Material Output's Volume socket and nothing on its Surface,
///   so the box has no surface of its own in Cycles and in Eevee. Its attribute inputs are emptied:
///   the box has no grids, and the density, colour and temperature are the same everywhere in it.
///
/// @return  False, with a warning and nothing written, when an edge of the box is not longer than 0.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportFog(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* fog,
	const maxon::String& meshId, const maxon::String& materialId);

/// Whether @p fog varies its density by a noise (its Noise tab): it is then written by ExportFogNoise().
maxon::Bool HasFogNoise(const cinema::BaseObject* fog);

//----------------------------------------------------------------------------------------
/// Writes a Cycles Fog whose density a noise varies: Blender has none of Cinema 4D's noises, so the
/// plugin samples the noise itself (the SDK's C4DNoise, the functions the Noise shader uses) at the
/// centres of a regular grid that fills the box, and sends the grid as a .vdb file:
///
/// - the grid: the fog's Resolution cells along the box's longest edge and as many of the same size
///   along the others as fit (at least one), each the noise at its centre mapped to 0..1 by the clips,
///   brightness and contrast; cells of 0 are left out (the file holds what has fog). Sampled in the
///   fog's own axes (Space "Object": the noise moves, turns and scales with the box) or at the cells'
///   places in the world (Space "World"), at the document's time times the Animation Speed.
/// - the file: saved as Volume Builder grids are (volume_export.h, SaveVolumeFile) and reused while
///   nothing it follows from changed: the noise's values, the size, the resolution, and the time while
///   the noise moves (Speed not 0), the fog's matrix in world space. A box that only moves (object space)
///   keeps its file.
/// - the material @p materialId: the Principled Volume of ExportFog() reading the grid "density", so the
///   fog's Density times the noise is the density per metre.
/// - the `volume` item @p id at @p mg, the fog's global matrix.
///
/// @return  False, with a warning and nothing written, when the box has no volume or the noise could not
///          be sampled or saved.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportFogNoise(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseDocument* doc,
	const cinema::BaseObject* fog, const cinema::Matrix& mg, const maxon::String& id, const maxon::String& materialId);

/// What the last ExportFogNoise() of this process measured, for tests and the reference: the grid's cells
/// per axis, the milliseconds spent sampling and saving (0 when the file was reused), whether it was, and
/// the noise type and seed it sampled.
struct FogNoiseStats
{
	maxon::Int32 cells[3] = { 0, 0, 0 };
	maxon::Int32 type = 0;
	maxon::Int32 seed = 0;
	maxon::Float64 sampleMs = 0.0;
	maxon::Float64 saveMs = 0.0;
	maxon::Bool reused = false;
};
FogNoiseStats GetLastFogNoiseStats();

} // namespace cyclesrender

#endif // CYCLESRENDER_FOG_EXPORT_H__
