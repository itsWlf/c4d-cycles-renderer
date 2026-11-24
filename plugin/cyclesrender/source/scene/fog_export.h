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

/// The voxels of a new fog's grid along the box's longest edge (Voxel Resolution).
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

/// The shader linked as @p fog's Texture (its own, inserted under it), or nullptr.
cinema::BaseShader* GetFogTexture(const cinema::BaseObject* fog, const cinema::BaseDocument* doc);

/// Whether @p fog varies its density across the box, by a noise (its Noise tab) or a texture (a shader
/// linked as its Texture): it is then written by ExportFogGrid().
maxon::Bool HasFogGrid(const cinema::BaseObject* fog, const cinema::BaseDocument* doc);

/// The voxels of @p fog's grid along X, Y and Z: Voxel Resolution cubes along the box's longest edge,
/// and as many of that size along the others as fit (at least one). All 0 for a box without volume.
void GetFogGridCells(const cinema::BaseObject* fog, maxon::Int32 (&cells)[3]);

//----------------------------------------------------------------------------------------
/// Writes a Cycles Fog whose density varies across the box: by one of Cinema 4D's noises (the Noise
/// tab), by a shader (the Texture), or both multiplied. Blender has neither, so the plugin samples them
/// itself at the centres of a regular grid that fills the box, and sends the grid as a .vdb file:
///
/// - the grid: GetFogGridCells(); each voxel is the product of what is on: the noise at its centre (the
///   SDK's C4DNoise, the functions the Noise shader uses) mapped to 0..1 by the clips, brightness and
///   contrast, sampled in the fog's own axes (Space "Object") or at the voxel's place in the world (Space
///   "World") at the document's time times the Animation Speed; and the texture's value there, the mean of
///   the colour BaseShader::Sample() gives, limited to 0..1. The texture sees the box as its unit cube:
///   a voxel's centre at 0..1 on each of the box's axes, U and V along the two axes of the Projection (V
///   downwards, so a picture stands upright as on a Cube's side) and W along the third, at the document's
///   time, without VolumeData (docs/reference/c4d-sdk.md, "The Cycles Fog object": what each shader reads
///   then). Voxels of 0 are left out (the file holds what has fog). Both are sampled on all cores.
/// - the file: saved as Volume Builder grids are (volume_export.h, SaveVolumeFile) and reused while
///   nothing it follows from changed: the noise's values, the size, the resolution, the time while the
///   noise moves (Speed not 0), the fog's matrix in world space; the texture's settings and those of every
///   shader inside it (ShaderFingerprint, materials/material_export.h), the projection, the document's
///   colour settings and, for a shader that changes over time by itself, the frame. A box that only moves
///   keeps its file (unless the noise is in world space).
/// - the material @p materialId: the Principled Volume of ExportFog() reading the grid "density", so the
///   fog's Density times the grid is the density per metre.
/// - the `volume` item @p id at @p mg, the fog's global matrix.
///
/// A texture that cannot be prepared (InitRender fails) is left out with a warning; the noise still counts.
/// @return  False, with a warning and nothing written, when the box has no volume or the grid could not
///          be sampled or saved.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportFogGrid(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseDocument* doc,
	const cinema::BaseObject* fog, const cinema::Matrix& mg, const maxon::String& id, const maxon::String& materialId);

/// What the last ExportFogGrid() of this process measured, for tests and the reference: the grid's cells
/// per axis, the milliseconds spent sampling and saving (0 when the file was reused), whether it was, what
/// was sampled (the noise and its type and seed, the texture), on which thread, and the voxels with fog.
struct FogNoiseStats
{
	maxon::Int32 cells[3] = { 0, 0, 0 };
	maxon::Int32 type = 0;
	maxon::Int32 seed = 0;
	maxon::Bool noise = false;
	maxon::Bool textured = false;
	maxon::Bool mainThread = false;
	maxon::Int64 filled = 0;
	maxon::Float64 sampleMs = 0.0;
	maxon::Float64 saveMs = 0.0;
	maxon::Bool reused = false;
};
FogNoiseStats GetLastFogNoiseStats();

} // namespace cyclesrender

#endif // CYCLESRENDER_FOG_EXPORT_H__
