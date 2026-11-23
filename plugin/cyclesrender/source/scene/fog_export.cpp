// SPDX-License-Identifier: Apache-2.0
// The Cycles Fog object as a box mesh and a volume material (fog_export.h).

#include "c4d.h"
#include "lib_noise.h"
#include "maxon/parallelfor.h"
#include "maxon/volume.h"
#include "maxon/volumeaccessors.h"
#include "maxon/volumetools.h"

#include "ocyclesfog.h"

#include "../materials/node_graph.h"
#include "../session/json_writer.h"
#include "fog_export.h"
#include "parameters.h"
#include "volume_export.h"

namespace cyclesrender
{

static maxon::Result<maxon::Bool> WriteFogMaterial(SceneWriter& writer, const DocumentColors& colors, const cinema::BaseObject* fog,
	const maxon::String& materialId, const maxon::String& densityGrid);

maxon::Bool IsFogSwitchedOn(const cinema::BaseObject* fog)
{
	return fog && fog->GetDeformMode();
}

// The box's corners by Blender's axes: corner i lies at +X when bit 0 is set, at +Y (Cinema 4D's +Z)
// with bit 1, at +Z (Cinema 4D's +Y) with bit 2. Each face lists its corners counter-clockwise seen
// from outside, Blender's order (protocol.md, `mesh`), so that the normals point out of the box: a
// volume is what lies behind the faces' backs.
static const maxon::Int32 BOX_FACES[6][4] = {
	{ 0, 2, 3, 1 },   // -Z (bottom)
	{ 4, 5, 7, 6 },   // +Z (top)
	{ 0, 1, 5, 4 },   // -Y
	{ 2, 6, 7, 3 },   // +Y
	{ 0, 4, 6, 2 },   // -X
	{ 1, 3, 7, 5 }    // +X
};

maxon::Result<maxon::Bool> ExportFog(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* fog,
	const maxon::String& meshId, const maxon::String& materialId)
{
	iferr_scope;

	const cinema::Vector size = GetVectorParameter(fog, CYCLESFOG_SIZE, cinema::Vector(FOG_DEFAULT_SIZE));
	if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0)
	{
		writer.Warn(FormatString("Fog \"@\": its box has no volume (Size @, @, @) and renders nothing.", fog->GetName(), size.x, size.y, size.z));
		return false;
	}

	// --- the mesh: six quads ----------------------------------------------------------------------
	maxon::Int cornersBlob = 0;
	maxon::Int sizesBlob = 0;
	maxon::Int32* const corners = writer.AllocInts(24, cornersBlob) iferr_return;
	maxon::Int32* const sizes = writer.AllocInts(6, sizesBlob) iferr_return;
	for (maxon::Int face = 0; face < 6; ++face)
	{
		for (maxon::Int c = 0; c < 4; ++c)
			corners[face * 4 + c] = BOX_FACES[face][c];
		sizes[face] = 4;
	}
	{
		JsonWriter& json = writer.BeginItem("mesh", meshId);
		json.Int("point_count", 8);
		json.Int("corner_verts", cornersBlob);
		json.Int("face_sizes", sizesBlob);
		json.Bool("smooth", false);
		json.Int("slots", 1);
		writer.EndItem();
	}

	// --- its points, in the fog's own axes ---------------------------------------------------------
	const cinema::Vector half = size * 0.5;
	maxon::Int pointsBlob = 0;
	maxon::Float32* const points = writer.AllocFloats(8 * 3, pointsBlob) iferr_return;
	for (maxon::Int i = 0; i < 8; ++i)
	{
		// Blender's X, Y, Z are Cinema 4D's X, Z, Y (coords.h).
		const cinema::Vector corner((i & 1) ? half.x : -half.x, (i & 4) ? half.y : -half.y, (i & 2) ? half.z : -half.z);
		coords.Point(corner, points + i * 3);
	}
	{
		JsonWriter& json = writer.BeginItem("points", meshId + ".points"_s);
		json.String("mesh", meshId);
		json.Int("points", pointsBlob);
		// When the mesh is built again its points are gone, so they must follow it.
		writer.SendWith(meshId);
		writer.EndItem();
	}

	return WriteFogMaterial(writer, colors, fog, materialId, maxon::String());
}

// The Principled Volume of the fog's values, reading @p densityGrid (or no grid at all).
static maxon::Result<maxon::Bool> WriteFogMaterial(SceneWriter& writer, const DocumentColors& colors, const cinema::BaseObject* fog,
	const maxon::String& materialId, const maxon::String& densityGrid)
{
	iferr_scope;

	// --- the material: a volume and no surface ---------------------------------------------------
	NodeGraph graph;
	const maxon::Int shader = graph.Add("ShaderNodeVolumePrincipled") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	// No grids but the noise's: the node's attribute inputs ("density", "temperature" by default) read
	// nothing else.
	graph.SetInputJson(shader, "Density Attribute", JsonQuoted(densityGrid));
	graph.SetInputJson(shader, "Color Attribute", JsonQuoted(maxon::String()));
	graph.SetInputJson(shader, "Temperature Attribute", JsonQuoted(maxon::String()));
	graph.SetInput(shader, "Color", colors.Input(GetVectorParameter(fog, CYCLESFOG_COLOR, cinema::Vector(0.5))));
	graph.SetInput(shader, "Density", GetFloatParameter(fog, CYCLESFOG_DENSITY, FOG_DEFAULT_DENSITY));
	graph.SetInput(shader, "Anisotropy", GetFloatParameter(fog, CYCLESFOG_ANISOTROPY, 0.0));
	graph.SetInput(shader, "Absorption Color", colors.Input(GetVectorParameter(fog, CYCLESFOG_ABSORPTION_COLOR, cinema::Vector(0.0))));
	graph.SetInput(shader, "Emission Strength", GetFloatParameter(fog, CYCLESFOG_EMISSION_STRENGTH, 0.0));
	graph.SetInput(shader, "Emission Color", colors.Input(GetVectorParameter(fog, CYCLESFOG_EMISSION_COLOR, cinema::Vector(1.0))));
	graph.SetInput(shader, "Blackbody Intensity", GetFloatParameter(fog, CYCLESFOG_BLACKBODY_INTENSITY, 0.0));
	graph.SetInput(shader, "Blackbody Tint", colors.Input(GetVectorParameter(fog, CYCLESFOG_BLACKBODY_TINT, cinema::Vector(1.0))));
	graph.SetInput(shader, "Temperature", GetFloatParameter(fog, CYCLESFOG_TEMPERATURE, 1000.0));
	graph.Link(NodeOutput { shader, "Volume"_s }, output, "Volume") iferr_return;
	{
		JsonWriter& json = writer.BeginItem("material", materialId);
		graph.Write(json);
		writer.EndItem();
	}
	return true;
}

// --- the noise -------------------------------------------------------------------------------------

maxon::Bool HasFogNoise(const cinema::BaseObject* fog)
{
	return GetBoolParameter(fog, CYCLESFOG_NOISE_ENABLE, false);
}

static FogNoiseStats g_lastNoise;
static maxon::Spinlock g_lastNoiseLock;

FogNoiseStats GetLastFogNoiseStats()
{
	maxon::ScopedLock lock(g_lastNoiseLock);
	return g_lastNoise;
}

namespace
{
/// The noise's settings as read from the fog, and the mapping of a noise value to 0..1.
struct FogNoise
{
	maxon::Int32 type = cinema::NOISE_NOISE;
	maxon::Bool world = false;
	cinema::Vector scale;           ///< document units per unit of the noise, per axis
	maxon::Float64 octaves = 5.0;
	maxon::Int32 seed = 665;
	maxon::Bool absolute = false;
	maxon::Float64 low = 0.0;
	maxon::Float64 high = 1.0;
	maxon::Float64 brightness = 0.0;
	maxon::Float64 contrast = 0.0;
	maxon::Float64 time = 0.0;      ///< the noise's time: the document's seconds times the speed
	maxon::Bool moving = false;     ///< the speed is not 0

	/// A noise value as the fog's share of its density: the clips stretch [low, high] to [0, 1], then
	/// the contrast scales around the middle and the brightness is added, each limited to [0, 1].
	maxon::Float64 Map(maxon::Float64 n) const
	{
		maxon::Float64 v = high > low ? (n - low) / (high - low) : (n >= high ? 1.0 : 0.0);
		v = maxon::Clamp01(v);
		v = maxon::Clamp01((v - 0.5) * (1.0 + contrast) + 0.5 + brightness);
		return v;
	}
};
}

maxon::Result<maxon::Bool> ExportFogNoise(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseDocument* doc,
	const cinema::BaseObject* fog, const cinema::Matrix& mg, const maxon::String& id, const maxon::String& materialId)
{
	iferr_scope;

	const cinema::Vector size = GetVectorParameter(fog, CYCLESFOG_SIZE, cinema::Vector(FOG_DEFAULT_SIZE));
	if (size.x <= 0.0 || size.y <= 0.0 || size.z <= 0.0)
	{
		writer.Warn(FormatString("Fog \"@\": its box has no volume (Size @, @, @) and renders nothing.", fog->GetName(), size.x, size.y, size.z));
		return false;
	}

	FogNoise noise;
	noise.type = GetIntParameter(fog, CYCLESFOG_NOISE_TYPE, cinema::NOISE_NOISE);
	noise.world = GetIntParameter(fog, CYCLESFOG_NOISE_SPACE, CYCLESFOG_NOISE_SPACE_OBJECT) == CYCLESFOG_NOISE_SPACE_WORLD;
	const maxon::Float64 globalScale = maxon::Max(1e-6, GetFloatParameter(fog, CYCLESFOG_NOISE_SCALE, 100.0));
	const cinema::Vector relative = GetVectorParameter(fog, CYCLESFOG_NOISE_RELATIVE_SCALE, cinema::Vector(1.0));
	noise.scale = cinema::Vector(maxon::Max(1e-6, globalScale * relative.x), maxon::Max(1e-6, globalScale * relative.y), maxon::Max(1e-6, globalScale * relative.z));
	noise.octaves = GetFloatParameter(fog, CYCLESFOG_NOISE_OCTAVES, 5.0);
	noise.seed = GetIntParameter(fog, CYCLESFOG_NOISE_SEED, 665);
	noise.absolute = GetBoolParameter(fog, CYCLESFOG_NOISE_ABSOLUTE, false);
	noise.low = GetFloatParameter(fog, CYCLESFOG_NOISE_LOW_CLIP, 0.0);
	noise.high = GetFloatParameter(fog, CYCLESFOG_NOISE_HIGH_CLIP, 1.0);
	noise.brightness = GetFloatParameter(fog, CYCLESFOG_NOISE_BRIGHTNESS, 0.0);
	noise.contrast = GetFloatParameter(fog, CYCLESFOG_NOISE_CONTRAST, 0.0);
	const maxon::Float64 speed = GetFloatParameter(fog, CYCLESFOG_NOISE_SPEED, 0.0);
	noise.moving = speed != 0.0;
	noise.time = noise.moving && doc ? doc->GetTime().Get() * speed : 0.0;
	if (noise.type == cinema::NOISE_NONE || noise.type == cinema::NOISE_UNKNOWN)
		noise.type = cinema::NOISE_NOISE;

	// The cells: cubes of the longest edge over the resolution, as many as fit along each edge, then
	// stretched a little so that they fill it exactly.
	const maxon::Int32 resolution = maxon::ClampValue(GetIntParameter(fog, CYCLESFOG_NOISE_RESOLUTION, FOG_DEFAULT_NOISE_RESOLUTION), maxon::Int32(1), maxon::Int32(1024));
	const maxon::Float64 longest = maxon::Max(size.x, maxon::Max(size.y, size.z));
	const maxon::Float64 edge = longest / maxon::Float64(resolution);
	const maxon::Int32 nx = maxon::Max(maxon::Int32(1), maxon::Int32(maxon::Floor(size.x / edge + 0.5)));
	const maxon::Int32 ny = maxon::Max(maxon::Int32(1), maxon::Int32(maxon::Floor(size.y / edge + 0.5)));
	const maxon::Int32 nz = maxon::Max(maxon::Int32(1), maxon::Int32(maxon::Floor(size.z / edge + 0.5)));
	const cinema::Vector cell(size.x / nx, size.y / ny, size.z / nz);
	const cinema::Vector first = -size * 0.5 + cell * 0.5;   // the centre of cell (0, 0, 0), in the fog's axes

	// Everything the grid follows from: the file is reused while it is the same, also by the copies of the
	// document a render makes.
	maxon::UInt64 key = 0x666F67ull;
	const maxon::Float64 numbers[] = { maxon::Float64(noise.type), noise.world ? 1.0 : 0.0, noise.scale.x, noise.scale.y, noise.scale.z, noise.octaves,
		maxon::Float64(noise.seed), noise.absolute ? 1.0 : 0.0, noise.low, noise.high, noise.brightness, noise.contrast, noise.time,
		size.x, size.y, size.z, maxon::Float64(resolution) };
	key = HashBytes(reinterpret_cast<const maxon::Char*>(numbers), SIZEOF(numbers), key);
	if (noise.world)
		key = HashBytes(reinterpret_cast<const maxon::Char*>(&mg), SIZEOF(mg), key);

	FogNoiseStats stats;
	stats.cells[0] = nx;
	stats.cells[1] = ny;
	stats.cells[2] = nz;
	stats.type = noise.type;
	stats.seed = noise.seed;
	cinema::Filename file = FindVolumeFile(id, key);
	stats.reused = file.IsPopulated();
	if (!stats.reused)
	{
		// Sampling: the noise at every cell's centre, slice by slice on all cores (the Noise shader samples
		// one noise from many render threads).
		const maxon::Float64 started = cinema::GeGetMilliSeconds();
		cinema::C4DNoise* sampler = cinema::C4DNoise::Alloc2(noise.seed);
		if (!sampler)
		{
			writer.Warn(FormatString("Fog \"@\": Cinema 4D's noise could not be made; the fog is left out.", fog->GetName()));
			return false;
		}
		finally
		{
			cinema::C4DNoise::Free(sampler);
		};
		sampler->InitFbm(21, 2.1, 0.5);
		maxon::BaseArray<maxon::Float32> values;
		values.Resize(maxon::Int(nx) * ny * nz) iferr_return;
		maxon::ParallelFor::Dynamic(0, maxon::Int(nz), [&](maxon::Int k)
		{
			for (maxon::Int32 j = 0; j < ny; ++j)
			{
				for (maxon::Int32 i = 0; i < nx; ++i)
				{
					const cinema::Vector local = first + cinema::Vector(cell.x * i, cell.y * j, cell.z * maxon::Float64(k));
					const cinema::Vector at = noise.world ? mg * local : local;
					const cinema::Vector p(at.x / noise.scale.x, at.y / noise.scale.y, at.z / noise.scale.z);
					const maxon::Float64 n = sampler->Noise(cinema::NoiseType(noise.type), false, p, noise.time, noise.octaves, noise.absolute);
					values[(k * ny + j) * nx + i] = maxon::Float32(noise.Map(n));
				}
			}
		});
		stats.sampleMs = cinema::GeGetMilliSeconds() - started;

		// The grid: cell (i, j, k) at its centre in the fog's axes, which the item's matrix places.
		const maxon::Float64 saving = cinema::GeGetMilliSeconds();
		maxon::Volume volume = maxon::VolumeToolsInterface::CreateNewFloat32Volume(0.0f) iferr_return;
		maxon::Matrix transform;
		transform.sqmat.v1 = maxon::Vector(cell.x, 0.0, 0.0);
		transform.sqmat.v2 = maxon::Vector(0.0, cell.y, 0.0);
		transform.sqmat.v3 = maxon::Vector(0.0, 0.0, cell.z);
		transform.off = maxon::Vector(first.x, first.y, first.z);
		volume.SetGridTransform(transform) iferr_return;
		volume.SetGridClass(maxon::GRIDCLASS::FOG) iferr_return;
		volume.SetGridName("density"_s);
		{
			maxon::GridAccessorRef<maxon::Float32> access = maxon::GridAccessorCreationInterface::Create<maxon::Float32>() iferr_return;
			access.InitWithWriteAccess(volume) iferr_return;
			for (maxon::Int32 k = 0; k < nz; ++k)
			{
				for (maxon::Int32 j = 0; j < ny; ++j)
				{
					for (maxon::Int32 i = 0; i < nx; ++i)
					{
						const maxon::Float32 value = values[(maxon::Int(k) * ny + j) * nx + i];
						if (value > 0.0f)
						{
							access.SetValue(maxon::IntVector32(i, j, k), value) iferr_return;
						}
					}
				}
			}
		}
		maxon::BaseArray<maxon::Volume> volumes;
		volumes.Append(std::move(volume)) iferr_return;
		iferr (file = SaveVolumeFile(id, volumes, key))
		{
			writer.Warn(FormatString("Fog \"@\": its noise could not be written for Blender and the fog is left out. @", fog->GetName(), err));
			return false;
		}
		stats.saveMs = cinema::GeGetMilliSeconds() - saving;
	}
	{
		maxon::ScopedLock lock(g_lastNoiseLock);
		g_lastNoise = stats;
	}

	WriteFogMaterial(writer, colors, fog, materialId, "density"_s) iferr_return;
	maxon::Float64 matrix[16];
	coords.RawMatrix(mg, 1.0, matrix);
	JsonWriter& json = writer.BeginItem("volume", id);
	json.String("path", file.GetString());
	json.Floats("matrix", matrix, 16);
	json.BeginArray("materials");
	json.String(nullptr, materialId);
	json.EndArray();
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
