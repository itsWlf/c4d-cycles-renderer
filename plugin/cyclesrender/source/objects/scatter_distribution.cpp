// SPDX-License-Identifier: Apache-2.0
// The Cycles Scatter object's copies (scatter_distribution.h). Reading order: the random numbers, the
// surface as triangles, the samplers (shaders, the field list), the candidates and the restrictions, the
// four distributions, the copies' axes and the shaders that vary them, the effectors, and last what the
// copies depend on (ScatterDependencies).

#include "c4d.h"
#include "c4d_baseeffectordata.h"
#include "c4d_fielddata.h"
#include "customgui_field.h"
#include "customgui_inexclude.h"
#include "maxon/file_utilities.h"
#include "maxon/hashmap.h"
#include "ofalloff_panel.h"
#include "ocyclesscatter.h"

#include "../scene/parameters.h"
#include "scatter_distribution.h"

namespace cyclesrender
{

// --- random numbers ----------------------------------------------------------------------------
//
// The plugin's own, not Cinema 4D's Random class: a hash of the seed, the candidate's number and what the
// number is drawn for (its stream). Integer arithmetic only, so the same in every session, Cinema 4D
// version and compiler; and each candidate draws its own numbers, so that removing one copy (a
// restriction, keep-away) never moves another.

static maxon::UInt64 Mix64(maxon::UInt64 x)
{
	// SplitMix64's finaliser.
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return x;
}

// What a candidate's random numbers are drawn for.
static const maxon::UInt64 DRAW_TRIANGLE = 1;
static const maxon::UInt64 DRAW_U = 2;
static const maxon::UInt64 DRAW_V = 3;
static const maxon::UInt64 DRAW_DENSITY = 4;
static const maxon::UInt64 DRAW_FIELD = 5;
static const maxon::UInt64 DRAW_CHILD = 6;
static const maxon::UInt64 DRAW_AREA = 7;
static const maxon::UInt64 DRAW_WEIGHT = 8;

// A number in [0, 1).
static maxon::Float64 Draw(maxon::UInt64 seed, maxon::UInt64 index, maxon::UInt64 stream)
{
	const maxon::UInt64 hash = Mix64(Mix64(seed * 0x9E3779B97F4A7C15ull + stream) ^ (index * 0xD1B54A32D192ED03ull + 0x632BE59BD9B4E019ull));
	return maxon::Float64(hash >> 11) * (1.0 / 9007199254740992.0);
}

static maxon::UInt64 MixValue(maxon::UInt64 hash, maxon::UInt64 value)
{
	return Mix64(hash ^ (value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2)));
}

static maxon::UInt64 MixFloat(maxon::UInt64 hash, maxon::Float64 value)
{
	return MixValue(hash, *reinterpret_cast<const maxon::UInt64*>(&value));
}

static maxon::UInt64 MixMatrix(maxon::UInt64 hash, const cinema::Matrix& m)
{
	const cinema::Vector parts[4] = { m.off, m.sqmat.v1, m.sqmat.v2, m.sqmat.v3 };
	for (const cinema::Vector& v : parts)
	{
		hash = MixFloat(hash, v.x);
		hash = MixFloat(hash, v.y);
		hash = MixFloat(hash, v.z);
	}
	return hash;
}

// --- the surface ------------------------------------------------------------------------------

namespace
{
/// The surface's polygons as triangles, and its points, in the scatter object's axes.
struct Surface
{
	maxon::BaseArray<cinema::Vector> corners;         ///< 3 per triangle
	maxon::BaseArray<cinema::Vector> cornerUvs;       ///< 3 per triangle (Cinema 4D's UVW, v downwards)
	maxon::BaseArray<maxon::Float32> cornerWeights;   ///< 3 per triangle: the vertex map's, or 1
	maxon::BaseArray<cinema::Vector> normals;         ///< 1 per triangle, unit length
	maxon::BaseArray<maxon::Bool> hasUv;              ///< 1 per triangle
	maxon::BaseArray<maxon::Float64> cumulative;      ///< the area of the triangles up to and with this one
	maxon::BaseArray<maxon::Float64> areas;           ///< 1 per triangle
	maxon::Float64 largestArea = 0.0;
	maxon::BaseArray<cinema::Vector> points;          ///< the surface's points (Vertices)
	maxon::BaseArray<cinema::Vector> pointNormals;
	maxon::BaseArray<cinema::Vector> pointUvs;
	maxon::BaseArray<maxon::Float32> pointWeights;
	maxon::BaseArray<maxon::Bool> pointHasUv;
	cinema::Vector low = cinema::Vector(0.0);         ///< bounds of the points
	cinema::Vector high = cinema::Vector(0.0);
	maxon::Bool vertexMapFound = false;
};

/// A polygon object of the surface and its global matrix.
struct MeshPlace
{
	const cinema::PolygonObject* mesh = nullptr;
	cinema::Matrix mg;
};

/// A place a copy may go, before the restrictions.
struct Candidate
{
	cinema::Vector p;
	cinema::Vector n;
	cinema::Vector uv;
	maxon::Float32 weight = 1.0f;
	maxon::UInt64 index = 0;      ///< its number, which its random numbers are drawn by
	maxon::Bool areaKept = true;  ///< Surface and Poisson: its triangle's share of the area keeps it (SurfaceCandidate)
};

/// A copy placed, before its axes are made.
struct Placed
{
	cinema::Vector p;
	cinema::Vector n;
	cinema::Vector uv;
	maxon::UInt64 index = 0;
	maxon::Float64 field = 1.0;   ///< the object's field list at its place (1 without one)
};
}

// The polygon objects of @p op as the exporter's walk finds them (scene_exporter.cpp, Walk): a deform cache
// or a cache stands for the object, and an object of the document consumed by a generator of the surface
// (BIT_CONTROLOBJECT) is part of that generator's cache, not a surface of its own. Without @p deformed the
// deform caches are passed over: the surface at rest.
static maxon::Result<void> CollectMeshes(const cinema::BaseObject* op, const cinema::Matrix& mg, const cinema::BaseObject* root, maxon::Bool inDocument,
	maxon::BaseArray<MeshPlace>& out, maxon::Bool deformed = true)
{
	iferr_scope;
	if (inDocument && op != root && op->GetBit(BIT_CONTROLOBJECT))
		return maxon::OK;
	const cinema::BaseObject* cache = deformed ? op->GetDeformCache() : nullptr;
	if (!cache)
		cache = op->GetCache();
	if (cache)
	{
		CollectMeshes(cache, cache == op->GetDeformCache() ? mg : mg * cache->GetMl(), root, false, out, deformed) iferr_return;
	}
	else if (op->GetType() == Opolygon)
	{
		MeshPlace place;
		place.mesh = static_cast<const cinema::PolygonObject*>(op);
		place.mg = mg;
		out.Append(place) iferr_return;
	}
	for (const cinema::BaseObject* child = op->GetDown(); child; child = child->GetNext())
	{
		CollectMeshes(child, mg * child->GetMl(), root, inDocument, out, deformed) iferr_return;
	}
	return maxon::OK;
}

// The triangles of @p meshes in the order BuildSurface makes them ((a, b, c), then (a, c, d) of a quad), as their
// areas in the axes @p toScatter leads to.
static maxon::Result<void> TriangleAreas(const maxon::BaseArray<MeshPlace>& meshes, const cinema::Matrix& toScatter, maxon::BaseArray<maxon::Float64>& areas)
{
	iferr_scope;
	for (const MeshPlace& place : meshes)
	{
		const cinema::PolygonObject* const mesh = place.mesh;
		const maxon::Int32 pointCount = mesh->GetPointCount();
		const cinema::Vector* const points = mesh->GetPointR();
		const cinema::CPolygon* const polygons = mesh->GetPolygonR();
		if (pointCount <= 0 || !points)
			continue;
		const cinema::Matrix m = toScatter * place.mg;
		for (maxon::Int32 k = 0; k < mesh->GetPolygonCount() && polygons; ++k)
		{
			const cinema::CPolygon& polygon = polygons[k];
			const maxon::Int32 index[4] = { polygon.a, polygon.b, polygon.c, polygon.d };
			for (maxon::Int t = 0; t < (polygon.c == polygon.d ? 1 : 2); ++t)
			{
				const maxon::Int corner[3] = { 0, t == 0 ? 1 : 2, t == 0 ? 2 : 3 };
				maxon::Bool valid = true;
				for (maxon::Int c = 0; c < 3; ++c)
					valid = valid && index[corner[c]] >= 0 && index[corner[c]] < pointCount;
				if (!valid)
					continue;
				const cinema::Vector a = m * points[index[corner[0]]];
				areas.Append(0.5 * Cross(m * points[index[corner[1]]] - a, m * points[index[corner[2]]] - a).GetLength()) iferr_return;
			}
		}
	}
	return maxon::OK;
}

static const cinema::UVWTag* FindUvwTag(const cinema::BaseObject* op)
{
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (tag->GetType() == Tuvw)
			return static_cast<const cinema::UVWTag*>(tag);
	}
	return nullptr;
}

// The surface @p root (an object of the document) in the axes of the scatter object, whose global matrix is
// @p scatterMg. The vertex map counts on a mesh it belongs to: the tag's own polygon object, or the surface's
// deformed copy with as many points.
static maxon::Result<void> BuildSurface(const cinema::BaseObject* root, const cinema::Matrix& scatterMg, const cinema::VertexMapTag* vertexMap, Surface& surface)
{
	iferr_scope;
	maxon::BaseArray<MeshPlace> meshes;
	CollectMeshes(root, root->GetMg(), root, true, meshes) iferr_return;
	const cinema::Matrix toScatter = ~scatterMg;
	maxon::Float64 total = 0.0;
	maxon::Bool first = true;
	for (const MeshPlace& place : meshes)
	{
		const cinema::PolygonObject* const mesh = place.mesh;
		const maxon::Int32 pointCount = mesh->GetPointCount();
		const maxon::Int32 polygonCount = mesh->GetPolygonCount();
		const cinema::Vector* const source = mesh->GetPointR();
		const cinema::CPolygon* const polygons = mesh->GetPolygonR();
		if (pointCount <= 0 || !source)
			continue;
		const cinema::Matrix m = toScatter * place.mg;
		const cinema::UVWTag* const uvwTag = FindUvwTag(mesh);
		const cinema::ConstUVWHandle uvw = uvwTag && uvwTag->GetDataCount() == polygonCount ? uvwTag->GetDataAddressR() : nullptr;
		const maxon::Float32* weights = nullptr;
		if (vertexMap && vertexMap->GetDataCount() == pointCount && (vertexMap->GetObject() == mesh || vertexMap->GetObject() == root))
		{
			weights = vertexMap->GetDataAddressR();
			surface.vertexMapFound = true;
		}

		const maxon::Int firstPoint = surface.points.GetCount();
		for (maxon::Int32 i = 0; i < pointCount; ++i)
		{
			const cinema::Vector p = m * source[i];
			surface.points.Append(p) iferr_return;
			surface.pointNormals.Append(cinema::Vector(0.0)) iferr_return;
			surface.pointUvs.Append(cinema::Vector(0.0)) iferr_return;
			surface.pointHasUv.Append(false) iferr_return;
			surface.pointWeights.Append(weights ? weights[i] : 1.0f) iferr_return;
			if (first)
			{
				surface.low = surface.high = p;
				first = false;
			}
			surface.low = cinema::Vector(maxon::Min(surface.low.x, p.x), maxon::Min(surface.low.y, p.y), maxon::Min(surface.low.z, p.z));
			surface.high = cinema::Vector(maxon::Max(surface.high.x, p.x), maxon::Max(surface.high.y, p.y), maxon::Max(surface.high.z, p.z));
		}
		for (maxon::Int32 k = 0; k < polygonCount && polygons; ++k)
		{
			const cinema::CPolygon& polygon = polygons[k];
			const maxon::Int32 index[4] = { polygon.a, polygon.b, polygon.c, polygon.d };
			cinema::UVWStruct uv;
			if (uvw)
				cinema::UVWTag::Get(uvw, k, uv);
			const cinema::Vector uvs[4] = { uv.a, uv.b, uv.c, uv.d };
			const maxon::Int triangleCount = polygon.c == polygon.d ? 1 : 2;
			for (maxon::Int t = 0; t < triangleCount; ++t)
			{
				// (a, b, c) and (a, c, d): the corners of the quad's two triangles, in its winding.
				const maxon::Int corner[3] = { 0, t == 0 ? 1 : 2, t == 0 ? 2 : 3 };
				maxon::Bool valid = true;
				for (maxon::Int c = 0; c < 3; ++c)
					valid = valid && index[corner[c]] >= 0 && index[corner[c]] < pointCount;
				if (!valid)
					continue;
				const cinema::Vector a = surface.points[firstPoint + index[corner[0]]];
				const cinema::Vector b = surface.points[firstPoint + index[corner[1]]];
				const cinema::Vector c = surface.points[firstPoint + index[corner[2]]];
				// Cinema 4D's face normal (CalcFaceNormal): (b - a) x (c - a).
				// A degenerate triangle keeps its place in the order (its area is 0: no candidate is kept on it), so
				// that the triangles are numbered alike on every frame.
				const cinema::Vector cross = Cross(b - a, c - a);
				const maxon::Float64 length = cross.GetLength();
				for (maxon::Int i = 0; i < 3; ++i)
				{
					const maxon::Int point = firstPoint + index[corner[i]];
					surface.corners.Append(surface.points[point]) iferr_return;
					surface.cornerUvs.Append(uvs[corner[i]]) iferr_return;
					surface.cornerWeights.Append(surface.pointWeights[point]) iferr_return;
					surface.pointNormals[point] += cross;
					if (uvw && !surface.pointHasUv[point])
					{
						surface.pointUvs[point] = uvs[corner[i]];
						surface.pointHasUv[point] = true;
					}
				}
				surface.normals.Append(length > 0.0 ? cross / length : cinema::Vector(0.0, 1.0, 0.0)) iferr_return;
				surface.hasUv.Append(uvw != nullptr) iferr_return;
				total += 0.5 * length;
				surface.cumulative.Append(total) iferr_return;
				surface.areas.Append(0.5 * length) iferr_return;
				surface.largestArea = maxon::Max(surface.largestArea, 0.5 * length);
			}
		}
	}
	// The share of the area each triangle keeps candidates by is the surface's at rest, before its deformers: a
	// candidate's place is a triangle and barycentric weights, which ride along on a deformed surface, and whether it
	// is kept then does not change with the deformation. (Points moved otherwise, by point level animation or a
	// generator, have no rest: there the areas of the moment count.)
	maxon::BaseArray<MeshPlace> rest;
	CollectMeshes(root, root->GetMg(), root, true, rest, false) iferr_return;
	maxon::BaseArray<maxon::Float64> restAreas;
	TriangleAreas(rest, toScatter, restAreas) iferr_return;
	if (restAreas.GetCount() == surface.areas.GetCount())
	{
		surface.largestArea = 0.0;
		for (maxon::Int i = 0; i < restAreas.GetCount(); ++i)
		{
			surface.areas[i] = restAreas[i];
			surface.largestArea = maxon::Max(surface.largestArea, restAreas[i]);
		}
	}
	for (cinema::Vector& n : surface.pointNormals)
	{
		const maxon::Float64 length = n.GetLength();
		n = length > 0.0 ? n / length : cinema::Vector(0.0, 1.0, 0.0);
	}
	return maxon::OK;
}

// Where a copy without UVs samples a shader: the surface's bounds seen from above (+Y of the scatter
// object), u along X, v along -Z, so that a picture lies on the ground as in Cinema 4D's top view.
static cinema::Vector PlanarUv(const Surface& surface, const cinema::Vector& p)
{
	const cinema::Vector size = surface.high - surface.low;
	return cinema::Vector(size.x > 0.0 ? (p.x - surface.low.x) / size.x : 0.0, size.z > 0.0 ? (surface.high.z - p.z) / size.z : 0.0, 0.0);
}

// --- samplers -------------------------------------------------------------------------------------

namespace
{
/// One of the object's shaders, sampled as a grey value 0..1 (the mean of its channels, clamped) at a UV.
class ShaderSampler
{
public:
	ShaderSampler() = default;
	~ShaderSampler() { Free(); }
	MAXON_DISALLOW_COPY_AND_ASSIGN(ShaderSampler);

	/// @return  False when there is no shader or it cannot render.
	maxon::Bool Init(cinema::BaseShader* shader, const cinema::BaseDocument* doc)
	{
		Free();
		if (!shader || !doc)
			return false;
		const cinema::InitRenderStruct irs(doc);
		if (shader->InitRender(irs) != cinema::INITRENDERRESULT::OK)
			return false;
		_shader = shader;
		_time = doc->GetTime().Get();
		return true;
	}

	maxon::Bool IsActive() const { return _shader != nullptr; }

	maxon::Float64 Sample(const cinema::Vector& uv, const cinema::Vector& n) const
	{
		if (!_shader)
			return 1.0;
		cinema::ChannelData cd;
		cd.p = uv;
		cd.n = n;
		cd.d = cinema::Vector(0.0);
		cd.t = _time;
		cd.texflag = TEX_TILE;
		cd.vd = nullptr;
		cd.off = 0.0;
		cd.scale = 0.0;
		const cinema::Vector color = _shader->Sample(&cd);
		return maxon::Clamp01((color.x + color.y + color.z) / 3.0);
	}

	void Free()
	{
		if (_shader)
			_shader->FreeRender();
		_shader = nullptr;
	}

private:
	cinema::BaseShader* _shader = nullptr;
	maxon::Float64 _time = 0.0;
};
}

// The object's own field list, or null when it has no layer to sample.
static const cinema::FieldList* GetFieldList(const cinema::BaseObject* scatter, cinema::GeData& holder)
{
	holder = GetParameterData(scatter, CYCLESSCATTER_FIELDS);
	const cinema::FieldList* const list = holder.GetCustomDataType<cinema::FieldList>();
	return list && list->HasContent() ? list : nullptr;
}

// The field list's value at @p positions (in the scatter's axes, which @p toWorld turns into the world).
static maxon::Result<void> SampleFields(const cinema::FieldList* list, const cinema::BaseObject* caller, const cinema::Matrix& toWorld,
	const maxon::BaseArray<cinema::Vector>& positions, maxon::BaseArray<maxon::Float64>& values)
{
	iferr_scope;
	values.Reset();
	if (positions.IsEmpty())
		return maxon::OK;
	const cinema::FieldInput input(positions.GetFirst(), positions.GetCount(), toWorld);
	const cinema::FieldOutput output = list->SampleListSimple(*caller, input, cinema::FIELDSAMPLE_FLAG::VALUE) iferr_return;
	values.Resize(positions.GetCount()) iferr_return;
	for (maxon::Int i = 0; i < positions.GetCount(); ++i)
		values[i] = i < output._value.GetCount() ? maxon::Clamp01(output._value[i]) : 1.0;
	return maxon::OK;
}

// --- keep-away -------------------------------------------------------------------------------------

namespace
{
/// The copies placed so far in cells of a grid as large as the distance: whether a new one would be closer to
/// one of them than the distance looks at 27 cells.
class KeepAwayGrid
{
public:
	explicit KeepAwayGrid(maxon::Float64 distance) : _distance(distance) { }

	maxon::Bool IsActive() const { return _distance > 0.0; }

	maxon::Bool IsNear(const cinema::Vector& p) const
	{
		if (!IsActive())
			return false;
		const maxon::Int64 cx = Cell(p.x);
		const maxon::Int64 cy = Cell(p.y);
		const maxon::Int64 cz = Cell(p.z);
		const maxon::Float64 limit = _distance * _distance;
		for (maxon::Int64 dx = -1; dx <= 1; ++dx)
		{
			for (maxon::Int64 dy = -1; dy <= 1; ++dy)
			{
				for (maxon::Int64 dz = -1; dz <= 1; ++dz)
				{
					const auto* const head = _heads.Find(Key(cx + dx, cy + dy, cz + dz));
					for (maxon::Int i = head ? head->GetValue() : NOTOK; i != NOTOK; i = _next[i])
					{
						if ((_points[i] - p).GetSquaredLength() < limit)
							return true;
					}
				}
			}
		}
		return false;
	}

	maxon::Result<void> Add(const cinema::Vector& p)
	{
		iferr_scope;
		if (!IsActive())
			return maxon::OK;
		const maxon::Int index = _points.GetCount();
		_points.Append(p) iferr_return;
		const maxon::UInt64 key = Key(Cell(p.x), Cell(p.y), Cell(p.z));
		if (auto* const head = _heads.Find(key))
		{
			_next.Append(head->GetValue()) iferr_return;
			head->SetValue(index);
		}
		else
		{
			_next.Append(maxon::Int(NOTOK)) iferr_return;
			_heads.Insert(key, index) iferr_return;
		}
		return maxon::OK;
	}

private:
	maxon::Int64 Cell(maxon::Float64 v) const { return maxon::Int64(maxon::Floor(v / _distance)); }
	static maxon::UInt64 Key(maxon::Int64 x, maxon::Int64 y, maxon::Int64 z)
	{
		return Mix64(maxon::UInt64(x) * 0x9E3779B97F4A7C15ull ^ Mix64(maxon::UInt64(y) * 0xC2B2AE3D27D4EB4Full ^ Mix64(maxon::UInt64(z))));
	}

	maxon::Float64 _distance;
	maxon::HashMap<maxon::UInt64, maxon::Int> _heads;
	maxon::BaseArray<maxon::Int> _next;
	maxon::BaseArray<cinema::Vector> _points;
};
}

// --- the distributions ----------------------------------------------------------------------------

namespace
{
/// The settings the distributions read, once.
struct Settings
{
	maxon::Int32 mode = CYCLESSCATTER_MODE_SURFACE;
	maxon::UInt64 seed = 0;
	maxon::Int count = 0;
	maxon::Float64 poissonRadius = 0.0;
	maxon::Int poissonSamples = 0;
	maxon::Float64 keepAway = 0.0;
	maxon::Float64 normalAlign = 0.0;
	cinema::Vector up = cinema::Vector(0.0, 1.0, 0.0);
	maxon::Int32 childOrder = CYCLESSCATTER_CHILD_ORDER_RANDOM;
	maxon::Float64 vertexMapLimit = 0.0;
	maxon::Float64 densityMin = 0.0;
	maxon::Float64 densityMax = 1.0;
	maxon::Float64 normalThreshold = maxon::PI;
	maxon::Float64 positionDistance = 0.0;
	maxon::Float64 scaleMin = 1.0;
	maxon::Float64 scaleMax = 1.0;
	maxon::Float64 rotationAngle = 0.0;
	maxon::Int32 fieldsAffect = CYCLESSCATTER_FIELDS_AFFECT_DENSITY;
	maxon::Bool removeCopies = false;
};
}

// Candidates are taken in blocks: the shaders and the field list are sampled for a block at once.
static const maxon::Int BLOCK = 4096;

// Surface with a fixed count: the candidates tried per copy asked for before it gives up (with fewer copies and a
// line in the console), and the least and most tried in all.
static const maxon::Int SURFACE_TRIES_PER_COPY = 16;
static const maxon::Int SURFACE_MIN_CANDIDATES = 100000;
static const maxon::Int SURFACE_MAX_CANDIDATES = 20000000;

// The most candidates a Poisson distribution tries before it stops: about 2 s here (measured in the test,
// docs/reference/c4d-sdk.md, "The Cycles Scatter object"); the console says when it was reached.
static const maxon::Int POISSON_MAX_CANDIDATES = 2000000;

// Candidate @p index of the Surface and Poisson distributions: a place fixed in the surface's own terms (a
// triangle by its number, a point in it by barycentric weights), so that it is the same on every frame and
// follows a surface whose points move. The triangle is chosen uniformly and kept with the share of its area in
// the largest triangle's (DRAW_AREA), which spreads the kept candidates evenly by area without a cumulative
// table, in which a change of one triangle's area would shift every choice; the point in it is uniform (the
// square root of one number spreads the other two evenly).
static Candidate SurfaceCandidate(const Surface& surface, maxon::UInt64 seed, maxon::UInt64 index)
{
	Candidate candidate;
	candidate.index = index;
	const maxon::Int triangles = surface.areas.GetCount();
	const maxon::Int t = maxon::Min(triangles - 1, maxon::Int(Draw(seed, index, DRAW_TRIANGLE) * maxon::Float64(triangles)));
	candidate.areaKept = Draw(seed, index, DRAW_AREA) * surface.largestArea < surface.areas[t];
	const maxon::Float64 r = maxon::Sqrt(Draw(seed, index, DRAW_U));
	const maxon::Float64 s = Draw(seed, index, DRAW_V);
	const maxon::Float64 wa = 1.0 - r;
	const maxon::Float64 wb = r * (1.0 - s);
	const maxon::Float64 wc = r * s;
	candidate.p = surface.corners[t * 3] * wa + surface.corners[t * 3 + 1] * wb + surface.corners[t * 3 + 2] * wc;
	candidate.n = surface.normals[t];
	candidate.uv = surface.hasUv[t] ? surface.cornerUvs[t * 3] * wa + surface.cornerUvs[t * 3 + 1] * wb + surface.cornerUvs[t * 3 + 2] * wc
		: PlanarUv(surface, candidate.p);
	candidate.weight = maxon::Float32(surface.cornerWeights[t * 3] * wa + surface.cornerWeights[t * 3 + 1] * wb + surface.cornerWeights[t * 3 + 2] * wc);
	return candidate;
}

static Candidate PointCandidate(const Surface& surface, maxon::UInt64 index)
{
	Candidate candidate;
	candidate.index = index;
	candidate.p = surface.points[maxon::Int(index)];
	candidate.n = surface.pointNormals[maxon::Int(index)];
	candidate.uv = surface.pointHasUv[maxon::Int(index)] ? surface.pointUvs[maxon::Int(index)] : PlanarUv(surface, candidate.p);
	candidate.weight = surface.pointWeights[maxon::Int(index)];
	return candidate;
}

// Places copies over @p surface. The candidates come in a fixed order (their numbers), each with fixed random
// thresholds of its own, and are decided in that order: kept by its triangle's area (Surface, Poisson), then
// allowed by the restrictions (the vertex map: none under Limit or at 0, else as likely as its weight; the slope;
// the density shader; the object's field list), then not too close to a copy placed before it (Keep Away,
// Poisson's radius). So the same seed gives the same copies on every frame, and when a weight changes over time a
// copy stays where it is as long as its place is still allowed: only the candidates whose test changes come or
// go (and, with a fixed count, the last copies make way for places gained before them).
// Surface makes Count copies: the first Count candidates that pass, however many it has to try (up to
// SURFACE_TRIES_PER_COPY per copy; the console says when fewer found room); with "Restrictions Remove Copies"
// the first Count candidates kept by area are judged and those that fail are left out. Vertices judges every
// point, Poisson tries until Samples candidates in a row are too close (the surface is full): both remove.
static maxon::Result<void> PlaceOnSurface(const Surface& surface, const Settings& settings, cinema::BaseShader* densityShader, const cinema::FieldList* fields,
	const cinema::BaseObject* scatter, const cinema::BaseDocument* doc, maxon::BaseArray<Placed>& placed, ScatterStats& stats,
	maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;
	const maxon::Bool vertices = settings.mode == CYCLESSCATTER_MODE_VERTICES;
	const maxon::Bool poisson = settings.mode == CYCLESSCATTER_MODE_POISSON;
	const maxon::Bool surfaceMode = !vertices && !poisson;
	if (vertices ? surface.points.IsEmpty() : surface.areas.IsEmpty() || surface.largestArea <= 0.0)
	{
		warnings.Append("its surface has no polygons."_s) iferr_return;
		return maxon::OK;
	}
	const maxon::Bool fixedCount = surfaceMode && !settings.removeCopies;
	const maxon::Int limit = vertices ? surface.points.GetCount() : poisson ? POISSON_MAX_CANDIDATES
		: maxon::Min(SURFACE_MAX_CANDIDATES, maxon::Max(SURFACE_MIN_CANDIDATES, settings.count * SURFACE_TRIES_PER_COPY));
	const maxon::Bool useVertexMap = surface.vertexMapFound;
	const maxon::Float64 cosThreshold = settings.normalThreshold >= maxon::PI - 1e-9 ? -2.0 : maxon::Cos(settings.normalThreshold);
	ShaderSampler density;
	const maxon::Bool densityOn = density.Init(densityShader, doc);
	const maxon::Bool fieldDensity = fields && settings.fieldsAffect != CYCLESSCATTER_FIELDS_AFFECT_SCALE;
	const maxon::Bool fieldScale = fields && settings.fieldsAffect != CYCLESSCATTER_FIELDS_AFFECT_DENSITY;
	KeepAwayGrid grid(poisson ? maxon::Max(settings.poissonRadius, settings.keepAway) : settings.keepAway);
	maxon::Int failures = 0;
	maxon::Int judged = 0;
	maxon::Bool done = surfaceMode && settings.count <= 0;
	maxon::Bool full = false;

	maxon::BaseArray<Candidate> block;
	maxon::BaseArray<cinema::Vector> positions;
	maxon::BaseArray<maxon::Float64> fieldValues;
	for (maxon::Int start = 0; start < limit && !done; start += BLOCK)
	{
		const maxon::Int end = maxon::Min(limit, start + BLOCK);
		block.Reset();
		positions.Reset();
		for (maxon::Int i = start; i < end; ++i)
		{
			const Candidate candidate = vertices ? PointCandidate(surface, maxon::UInt64(i)) : SurfaceCandidate(surface, settings.seed, maxon::UInt64(i));
			++stats.candidates;
			if (!candidate.areaKept)
				continue;
			block.Append(candidate) iferr_return;
			if (fields)
			{
				positions.Append(candidate.p) iferr_return;
			}
		}
		if (fields)
		{
			SampleFields(fields, scatter, scatter->GetMg(), positions, fieldValues) iferr_return;
			stats.fieldsSampled = true;
		}
		for (maxon::Int b = 0; b < block.GetCount() && !done; ++b)
		{
			const Candidate& candidate = block[b];
			// Removing: the first Count candidates kept by area are the ones judged.
			if (surfaceMode && !fixedCount && ++judged > settings.count)
			{
				done = true;
				break;
			}
			if (useVertexMap)
			{
				if (candidate.weight <= 0.0f || candidate.weight < settings.vertexMapLimit)
					continue;
				if (Draw(settings.seed, candidate.index, DRAW_WEIGHT) >= candidate.weight)
					continue;
			}
			if (Dot(candidate.n, settings.up) < cosThreshold)
				continue;
			if (densityOn)
			{
				const maxon::Float64 keep = settings.densityMin + (settings.densityMax - settings.densityMin) * density.Sample(candidate.uv, candidate.n);
				if (Draw(settings.seed, candidate.index, DRAW_DENSITY) >= keep)
					continue;
			}
			const maxon::Float64 field = fields && b < fieldValues.GetCount() ? fieldValues[b] : 1.0;
			if (fieldDensity && Draw(settings.seed, candidate.index, DRAW_FIELD) >= field)
				continue;
			if (grid.IsNear(candidate.p))
			{
				if (poisson && ++failures >= settings.poissonSamples)
				{
					full = true;
					done = true;
				}
				continue;
			}
			failures = 0;
			grid.Add(candidate.p) iferr_return;
			Placed copy;
			copy.p = candidate.p;
			copy.n = candidate.n;
			copy.uv = candidate.uv;
			copy.index = candidate.index;
			copy.field = fieldScale ? field : 1.0;
			placed.Append(copy) iferr_return;
			if (fixedCount && placed.GetCount() >= settings.count)
				done = true;
		}
	}
	if (poisson && !full)
	{
		warnings.Append(FormatString("the Poisson distribution stopped after @ tries before the surface was full (raise the Radius).", POISSON_MAX_CANDIDATES)) iferr_return;
	}
	if (fixedCount && placed.GetCount() < settings.count)
	{
		warnings.Append(FormatString("only @ of its @ copies found room after @ tries (the vertex map, shader, slope, fields or Keep Away leave too little of the surface).",
			placed.GetCount(), settings.count, stats.candidates)) iferr_return;
	}
	return maxon::OK;
}

// --- the CSV file ---------------------------------------------------------------------------------

// Reads one number at @p p (a sign, digits, a point, an exponent), moving @p p past it.
static maxon::Bool ReadNumber(const maxon::Char*& p, const maxon::Char* end, maxon::Float64& value)
{
	const maxon::Char* q = p;
	maxon::Float64 sign = 1.0;
	if (q < end && (*q == '-' || *q == '+'))
	{
		sign = *q == '-' ? -1.0 : 1.0;
		++q;
	}
	maxon::Float64 number = 0.0;
	maxon::Bool digits = false;
	while (q < end && *q >= '0' && *q <= '9')
	{
		number = number * 10.0 + maxon::Float64(*q - '0');
		digits = true;
		++q;
	}
	if (q < end && *q == '.')
	{
		++q;
		maxon::Float64 scale = 0.1;
		while (q < end && *q >= '0' && *q <= '9')
		{
			number += maxon::Float64(*q - '0') * scale;
			scale *= 0.1;
			digits = true;
			++q;
		}
	}
	if (!digits)
		return false;
	if (q < end && (*q == 'e' || *q == 'E'))
	{
		const maxon::Char* e = q + 1;
		maxon::Int32 exponentSign = 1;
		if (e < end && (*e == '-' || *e == '+'))
		{
			exponentSign = *e == '-' ? -1 : 1;
			++e;
		}
		maxon::Int32 exponent = 0;
		maxon::Bool exponentDigits = false;
		while (e < end && *e >= '0' && *e <= '9')
		{
			exponent = exponent * 10 + maxon::Int32(*e - '0');
			exponentDigits = true;
			++e;
		}
		if (exponentDigits)
		{
			number *= maxon::Pow(10.0, maxon::Float64(exponentSign * exponent));
			q = e;
		}
	}
	value = sign * number;
	p = q;
	return true;
}

// The file of frame @p frame of a sequence named by one of its files, @p name: the last run of digits in its file
// name (before the extension) replaced by the frame number, as wide as that run or wider when the number needs it,
// as Cinema 4D numbers the files of an image sequence ("points_0012.csv": frame 37 is "points_0037.csv").
// @return  False when the name has no digits to replace.
static maxon::Bool SequenceFileName(const cinema::Filename& name, maxon::Int32 frame, cinema::Filename& file)
{
	const maxon::String path = name.GetString();
	const maxon::Int length = path.GetLength();
	maxon::Int nameStart = 0;
	for (maxon::Int i = 0; i < length; ++i)
	{
		if (path[i] == '/' || path[i] == '\\')
			nameStart = i + 1;
	}
	maxon::Int extension = length;
	for (maxon::Int i = length - 1; i >= nameStart; --i)
	{
		if (path[i] == '.')
		{
			extension = i;
			break;
		}
	}
	maxon::Int digitsEnd = extension;
	while (digitsEnd > nameStart && !(path[digitsEnd - 1] >= '0' && path[digitsEnd - 1] <= '9'))
		--digitsEnd;
	maxon::Int digitsStart = digitsEnd;
	while (digitsStart > nameStart && path[digitsStart - 1] >= '0' && path[digitsStart - 1] <= '9')
		--digitsStart;
	if (digitsStart == digitsEnd)
		return false;
	maxon::String number = maxon::String::IntToString(maxon::Abs(frame));
	while (number.GetLength() < digitsEnd - digitsStart)
		number = "0"_s + number;
	if (frame < 0)
		number = "-"_s + number;
	file = cinema::Filename(path.GetPart(0, digitsStart) + number + path.GetPart(digitsEnd, length - digitsEnd));
	return true;
}

// One copy per line of the CSV file: x, y, z in document units and the scatter object's axes, then optionally
// the rotation H, P, B in degrees (Cinema 4D's order), then a uniform scale or three. Separators: commas,
// semicolons, tabs or spaces; a line with fewer than three numbers (a header, a comment) is skipped.
static maxon::Result<void> ReadCopiesFile(const cinema::BaseObject* scatter, const cinema::BaseDocument* doc, maxon::BaseArray<cinema::Matrix>& matrices,
	maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;
	cinema::Filename name = GetFilenameParameter(scatter, CYCLESSCATTER_FILE);
	if (!name.IsPopulated())
	{
		warnings.Append("its distribution is a CSV file, but no file is chosen."_s) iferr_return;
		return maxon::OK;
	}
	// A sequence: the file of the document's frame plus the offset.
	if (GetBoolParameter(scatter, CYCLESSCATTER_FILE_SEQUENCE, false))
	{
		const maxon::Int32 frame = (doc ? doc->GetTime().GetFrame(doc->GetFps()) : 0) + GetIntParameter(scatter, CYCLESSCATTER_FILE_FRAME_OFFSET, 0);
		cinema::Filename file;
		if (!SequenceFileName(name, frame, file))
		{
			warnings.Append(FormatString("its CSV file \"@\" is to be a sequence, but its name has no frame number; the file itself is read.", name.GetString())) iferr_return;
		}
		else
		{
			name = file;
		}
	}
	cinema::Filename resolved;
	if (!cinema::GenerateTexturePath(doc ? doc->GetDocumentPath() : cinema::Filename(), name, cinema::Filename(), &resolved) || !resolved.IsPopulated())
		resolved = name;
	const maxon::Url url = cinema::MaxonConvert(resolved, cinema::MAXONCONVERTMODE::NONE);
	maxon::BaseArray<maxon::Char> bytes;
	iferr (maxon::FileUtilities::ReadFileToMemory(url, bytes))
	{
		warnings.Append(FormatString("the file \"@\" could not be read (@); it makes no copies.", resolved.GetString(), err.GetMessage())) iferr_return;
		return maxon::OK;
	}
	const maxon::Char* p = bytes.GetFirst();
	const maxon::Char* const end = p + bytes.GetCount();
	maxon::Int skipped = 0;
	while (p && p < end)
	{
		const maxon::Char* lineEnd = p;
		while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r')
			++lineEnd;
		maxon::Float64 numbers[9] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
		maxon::Int count = 0;
		maxon::Bool other = false;
		const maxon::Char* q = p;
		while (q < lineEnd && count < 9)
		{
			if (*q == ',' || *q == ';' || *q == ' ' || *q == '\t')
			{
				++q;
				continue;
			}
			if (!ReadNumber(q, lineEnd, numbers[count]))
			{
				other = true;
				break;
			}
			++count;
		}
		if (count >= 3 && !other)
		{
			cinema::Matrix m;
			if (count >= 6)
				m = cinema::HPBToMatrix(cinema::Vector(maxon::DegToRad(numbers[3]), maxon::DegToRad(numbers[4]), maxon::DegToRad(numbers[5])), cinema::ROTATIONORDER::HPB);
			const cinema::Vector scale = count >= 9 ? cinema::Vector(numbers[6], numbers[7], numbers[8]) : count >= 7 ? cinema::Vector(numbers[6]) : cinema::Vector(1.0);
			m.sqmat.v1 *= scale.x;
			m.sqmat.v2 *= scale.y;
			m.sqmat.v3 *= scale.z;
			m.off = cinema::Vector(numbers[0], numbers[1], numbers[2]);
			matrices.Append(m) iferr_return;
		}
		else if (lineEnd > p)
		{
			++skipped;
		}
		p = lineEnd;
		while (p < end && (*p == '\n' || *p == '\r'))
			++p;
	}
	if (matrices.IsEmpty())
	{
		warnings.Append(FormatString("the file \"@\" holds no line of at least three numbers (x, y, z).", resolved.GetString())) iferr_return;
	}
	else if (skipped > 1)
	{
		warnings.Append(FormatString("@ lines of the file \"@\" are not three to nine numbers and were skipped.", skipped, resolved.GetString())) iferr_return;
	}
	return maxon::OK;
}

// --- the copies' axes, and the shaders that vary them --------------------------------------------------

// The copy's Y between the up vector (align 0) and the normal (align 1); its X as close to the scatter
// object's X as that allows (its Z where Y lies along X).
static cinema::Matrix AlignedMatrix(const cinema::Vector& p, const cinema::Vector& n, const cinema::Vector& up, maxon::Float64 align)
{
	cinema::Vector y = up * (1.0 - align) + n * align;
	if (y.GetLength() < 1e-9)
		y = up;
	y = y.GetNormalized();
	cinema::Vector x;
	cinema::Vector z;
	const cinema::Vector xRef(1.0, 0.0, 0.0);
	if (maxon::Abs(Dot(xRef, y)) < 0.999)
	{
		x = (xRef - y * Dot(xRef, y)).GetNormalized();
		z = Cross(x, y);
	}
	else
	{
		const cinema::Vector zRef(0.0, 0.0, 1.0);
		z = (zRef - y * Dot(zRef, y)).GetNormalized();
		x = Cross(y, z);
	}
	cinema::Matrix m;
	m.off = p;
	m.sqmat.v1 = x;
	m.sqmat.v2 = y;
	m.sqmat.v3 = z;
	return m;
}

// --- the effectors -------------------------------------------------------------------------------

// The arrays a Cloner's MoData has, which effectors read and write.
static const maxon::Int32 MODATA_ARRAYS[] = { cinema::MODATA_MATRIX, cinema::MODATA_COLOR, cinema::MODATA_SIZE, cinema::MODATA_UVW, cinema::MODATA_FLAGS,
	cinema::MODATA_WEIGHT, cinema::MODATA_CLONE, cinema::MODATA_TIME, cinema::MODATA_LASTMAT, cinema::MODATA_STARTMAT, cinema::MODATA_ALT_INDEX,
	cinema::MODATA_FALLOFF_WGT };

// The effectors of the list that are switched on, in its order: entries whose check in the list is on and
// whose own green check is on.
static maxon::Result<void> GetEffectors(const cinema::BaseObject* scatter, const cinema::BaseDocument* doc, maxon::BaseArray<cinema::BaseObject*>& effectors)
{
	iferr_scope;
	const cinema::GeData data = GetParameterData(scatter, CYCLESSCATTER_EFFECTORS);
	const cinema::InExcludeData* const list = data.GetCustomDataType<cinema::InExcludeData>();
	if (!list)
		return maxon::OK;
	for (maxon::Int32 i = 0; i < list->GetObjectCount(); ++i)
	{
		const cinema::BaseList2D* const entry = list->ObjectFromIndex(doc, i);
		if (!entry || !entry->IsInstanceOf(Obaseeffector) || (list->GetFlags(i) & 1) == 0)
			continue;
		const cinema::BaseObject* const effector = static_cast<const cinema::BaseObject*>(entry);
		if (!effector->GetDeformMode())
			continue;
		// Executing an effector changes the MoData handed to it, not the effector: the SDK's message takes it non-const.
		effectors.Append(const_cast<cinema::BaseObject*>(effector)) iferr_return;
	}
	return maxon::OK;
}

// Runs the effectors over the copies as a Cloner does: the copies as MoData (matrices in the scatter object's
// axes, MoGraph colour, UVW, clone offset by source), each effector sent MSG_EXECUTE_EFFECTOR with the scatter
// as the generator; then what they left: the matrices, colours, the sources from the clone offset, and only
// the copies still visible.
static maxon::Result<void> RunEffectors(cinema::BaseObject* scatter, const maxon::BaseArray<cinema::BaseObject*>& effectors, cinema::BaseThread* thread,
	maxon::Int sourceCount, const maxon::BaseArray<cinema::Vector>& uvs, ScatterCopies& copies, ScatterStats& stats, maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;
	const maxon::Int count = copies.matrices.GetCount();
	if (effectors.IsEmpty() || count == 0)
		return maxon::OK;
	cinema::MoData* md = cinema::MoData::Alloc();
	if (!md)
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	finally
	{
		cinema::MoData::Free(md);
	};
	for (const maxon::Int32 id : MODATA_ARRAYS)
	{
		if (md->GetArrayIndex(id) == NOTOK && md->AddArray(id, maxon::Int32(cinema::GetMoDataDefaultType(id))) == NOTOK)
			return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION, "the MoData's arrays could not be made"_s);
	}
	if (!md->SetCount(count))
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	{
		cinema::MDArray<cinema::Matrix> matrices = md->GetMatrixArray(cinema::MODATA_MATRIX);
		cinema::MDArray<cinema::Matrix> last = md->GetMatrixArray(cinema::MODATA_LASTMAT);
		cinema::MDArray<cinema::Matrix> start = md->GetMatrixArray(cinema::MODATA_STARTMAT);
		cinema::MDArray<cinema::Vector> colors = md->GetVectorArray(cinema::MODATA_COLOR);
		cinema::MDArray<cinema::Vector> sizes = md->GetVectorArray(cinema::MODATA_SIZE);
		cinema::MDArray<cinema::Vector> uvw = md->GetVectorArray(cinema::MODATA_UVW);
		cinema::MDArray<maxon::Int32> flags = md->GetLongArray(cinema::MODATA_FLAGS);
		cinema::MDArray<maxon::Int32> altIndex = md->GetLongArray(cinema::MODATA_ALT_INDEX);
		cinema::MDArray<maxon::Float64> weights = md->GetRealArray(cinema::MODATA_WEIGHT);
		cinema::MDArray<maxon::Float64> clones = md->GetRealArray(cinema::MODATA_CLONE);
		cinema::MDArray<maxon::Float64> times = md->GetRealArray(cinema::MODATA_TIME);
		cinema::MDArray<maxon::Float64> falloff = md->GetRealArray(cinema::MODATA_FALLOFF_WGT);
		if (!matrices.GetPointer() || !colors.GetPointer() || !flags.GetPointer() || !clones.GetPointer())
			return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "the MoData has no matrix, colour, flag or clone array"_s);
		for (maxon::Int i = 0; i < count; ++i)
		{
			matrices[i] = copies.matrices[i];
			last[i] = copies.matrices[i];
			start[i] = copies.matrices[i];
			colors[i] = copies.colors[i];
			sizes[i] = cinema::Vector(1.0);
			uvw[i] = i < uvs.GetCount() ? uvs[i] : cinema::Vector(0.0);
			flags[i] = MOGENFLAG_CLONE_ON;
			altIndex[i] = maxon::Int32(i);
			weights[i] = 0.0;
			// The middle of the source's share of 0..1, so that the floor of clone * sources is the source again.
			clones[i] = sourceCount > 0 ? (maxon::Float64(copies.sources[i]) + 0.5) / maxon::Float64(sourceCount) : 0.0;
			times[i] = 0.0;
			falloff[i] = 1.0;
		}
	}
	for (cinema::BaseObject* const effector : effectors)
	{
		cinema::Effector_PassData pass;
		pass.op = scatter;
		pass.md = md;
		pass.weight = 1.0;
		pass.thread = thread;
		if (!effector->Message(MSG_EXECUTE_EFFECTOR, &pass))
		{
			warnings.Append(FormatString("the effector \"@\" did not run on its copies.", effector->GetName())) iferr_return;
			continue;
		}
		++stats.effectorsRun;
	}
	const cinema::MDArray<cinema::Matrix> matrices = md->GetMatrixArray(cinema::MODATA_MATRIX);
	const cinema::MDArray<cinema::Vector> colors = md->GetVectorArray(cinema::MODATA_COLOR);
	const cinema::MDArray<maxon::Int32> flags = md->GetLongArray(cinema::MODATA_FLAGS);
	const cinema::MDArray<maxon::Float64> clones = md->GetRealArray(cinema::MODATA_CLONE);
	const maxon::Int after = md->GetCount();
	maxon::Int kept = 0;
	for (maxon::Int i = 0; i < count && i < after; ++i)
	{
		const maxon::Int32 flag = flags[i];
		if (!(flag & MOGENFLAG_CLONE_ON) || (flag & MOGENFLAG_DISABLE))
		{
			++stats.hidden;
			continue;
		}
		copies.matrices[kept] = matrices[i];
		copies.colors[kept] = colors[i];
		const maxon::Int source = sourceCount > 0 ? maxon::Int(maxon::Floor(clones[i] * maxon::Float64(sourceCount))) : 0;
		copies.sources[kept] = maxon::Int32(maxon::ClampValue(source, maxon::Int(0), maxon::Max(maxon::Int(0), sourceCount - 1)));
		++kept;
	}
	copies.matrices.Resize(kept) iferr_return;
	copies.colors.Resize(kept) iferr_return;
	copies.sources.Resize(kept) iferr_return;
	return maxon::OK;
}

// --- the whole -----------------------------------------------------------------------------------

static Settings ReadSettings(const cinema::BaseObject* scatter)
{
	Settings settings;
	settings.mode = GetIntParameter(scatter, CYCLESSCATTER_MODE, CYCLESSCATTER_MODE_SURFACE);
	settings.seed = maxon::UInt64(maxon::UInt32(GetIntParameter(scatter, CYCLESSCATTER_SEED, 0)));
	settings.count = maxon::Max(maxon::Int32(0), GetIntParameter(scatter, CYCLESSCATTER_COUNT, 0));
	settings.poissonRadius = maxon::Max(0.0, GetFloatParameter(scatter, CYCLESSCATTER_POISSON_RADIUS, 0.0));
	settings.poissonSamples = maxon::Max(maxon::Int32(1), GetIntParameter(scatter, CYCLESSCATTER_POISSON_SAMPLES, 1));
	settings.keepAway = maxon::Max(0.0, GetFloatParameter(scatter, CYCLESSCATTER_KEEP_AWAY, 0.0));
	settings.normalAlign = maxon::Clamp01(GetFloatParameter(scatter, CYCLESSCATTER_NORMAL_ALIGN, 0.0));
	const cinema::Vector up = GetVectorParameter(scatter, CYCLESSCATTER_UP_VECTOR, cinema::Vector(0.0, 1.0, 0.0));
	settings.up = up.GetLength() > 1e-12 ? up.GetNormalized() : cinema::Vector(0.0, 1.0, 0.0);
	settings.childOrder = GetIntParameter(scatter, CYCLESSCATTER_CHILD_ORDER, CYCLESSCATTER_CHILD_ORDER_RANDOM);
	settings.vertexMapLimit = maxon::Clamp01(GetFloatParameter(scatter, CYCLESSCATTER_VERTEX_MAP_LIMIT, 0.0));
	settings.densityMin = maxon::Clamp01(GetFloatParameter(scatter, CYCLESSCATTER_DENSITY_MIN, 0.0));
	settings.densityMax = maxon::Clamp01(GetFloatParameter(scatter, CYCLESSCATTER_DENSITY_MAX, 1.0));
	settings.normalThreshold = maxon::ClampValue(GetFloatParameter(scatter, CYCLESSCATTER_NORMAL_THRESHOLD, maxon::PI), 0.0, maxon::PI);
	settings.positionDistance = GetFloatParameter(scatter, CYCLESSCATTER_POSITION_DISTANCE, 0.0);
	settings.scaleMin = maxon::Max(0.0, GetFloatParameter(scatter, CYCLESSCATTER_SCALE_MIN, 1.0));
	settings.scaleMax = maxon::Max(0.0, GetFloatParameter(scatter, CYCLESSCATTER_SCALE_MAX, 1.0));
	settings.rotationAngle = GetFloatParameter(scatter, CYCLESSCATTER_ROTATION_ANGLE, 0.0);
	settings.fieldsAffect = GetIntParameter(scatter, CYCLESSCATTER_FIELDS_AFFECT, CYCLESSCATTER_FIELDS_AFFECT_DENSITY);
	settings.removeCopies = GetBoolParameter(scatter, CYCLESSCATTER_REMOVE_COPIES, false);
	return settings;
}

maxon::Result<void> ComputeScatter(cinema::BaseObject* scatter, const cinema::BaseDocument* doc, maxon::Int sourceCount, cinema::BaseThread* thread,
	ScatterCopies& copies, ScatterStats& stats, maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;
	copies.matrices.Reset();
	copies.colors.Reset();
	copies.sources.Reset();
	stats = ScatterStats();
	const maxon::Float64 started = cinema::GeGetMilliSeconds();
	const Settings settings = ReadSettings(scatter);
	cinema::GeData fieldHolder;
	const cinema::FieldList* const fields = GetFieldList(scatter, fieldHolder);
	const maxon::Bool fieldDensity = fields && settings.fieldsAffect != CYCLESSCATTER_FIELDS_AFFECT_SCALE;
	const maxon::Bool fieldScale = fields && settings.fieldsAffect != CYCLESSCATTER_FIELDS_AFFECT_DENSITY;

	maxon::BaseArray<cinema::Vector> uvs;
	if (settings.mode == CYCLESSCATTER_MODE_FILE)
	{
		ReadCopiesFile(scatter, doc, copies.matrices, warnings) iferr_return;
		maxon::BaseArray<maxon::Int> lines;
		stats.candidates = copies.matrices.GetCount();
		// The field list thins out and scales the copies of a file too; the restrictions and shaders of the
		// surface do not apply (there is no surface).
		if (fields && !copies.matrices.IsEmpty())
		{
			maxon::BaseArray<cinema::Vector> positions;
			for (const cinema::Matrix& m : copies.matrices)
			{
				positions.Append(m.off) iferr_return;
			}
			maxon::BaseArray<maxon::Float64> values;
			SampleFields(fields, scatter, scatter->GetMg(), positions, values) iferr_return;
			stats.fieldsSampled = true;
			maxon::Int kept = 0;
			for (maxon::Int i = 0; i < copies.matrices.GetCount(); ++i)
			{
				if (fieldDensity && Draw(settings.seed, maxon::UInt64(i), DRAW_FIELD) >= values[i])
					continue;
				cinema::Matrix m = copies.matrices[i];
				if (fieldScale)
					m.sqmat = m.sqmat * values[i];
				lines.Append(i) iferr_return;
				copies.matrices[kept++] = m;
			}
			copies.matrices.Resize(kept) iferr_return;
		}
		for (maxon::Int i = 0; i < copies.matrices.GetCount(); ++i)
		{
			// A copy's line of the file is what it is (which child it shows), whatever was left out before it.
			const maxon::Int line = i < lines.GetCount() ? lines[i] : i;
			const maxon::Int32 source = sourceCount <= 0 ? 0
				: settings.childOrder == CYCLESSCATTER_CHILD_ORDER_ITERATE ? maxon::Int32(line % sourceCount)
				: maxon::Int32(maxon::Min(sourceCount - 1, maxon::Int(Draw(settings.seed, maxon::UInt64(line), DRAW_CHILD) * maxon::Float64(sourceCount))));
			copies.sources.Append(source) iferr_return;
			copies.colors.Append(SCATTER_COPY_COLOR) iferr_return;
			uvs.Append(cinema::Vector(0.0)) iferr_return;
		}
	}
	else
	{
		const cinema::BaseList2D* const link = GetParameterData(scatter, CYCLESSCATTER_SURFACE).GetLink(doc);
		const cinema::BaseObject* const surfaceObject = link && link->IsInstanceOf(Obase) ? static_cast<const cinema::BaseObject*>(link) : nullptr;
		if (!surfaceObject)
		{
			warnings.Append("no surface is linked; it makes no copies."_s) iferr_return;
		}
		else if (surfaceObject == scatter)
		{
			warnings.Append("it cannot scatter over itself; link another object as the surface."_s) iferr_return;
		}
		else
		{
			const cinema::BaseList2D* const mapLink = GetParameterData(scatter, CYCLESSCATTER_VERTEX_MAP).GetLink(doc);
			const cinema::VertexMapTag* const vertexMap = mapLink && mapLink->GetType() == Tvertexmap ? static_cast<const cinema::VertexMapTag*>(mapLink) : nullptr;
			Surface surface;
			BuildSurface(surfaceObject, scatter->GetMg(), vertexMap, surface) iferr_return;
			stats.triangles = surface.normals.GetCount();
			if (vertexMap && !surface.vertexMapFound)
			{
				warnings.Append(FormatString("the vertex map \"@\" is not on its surface (or has another number of points); it is not used.", vertexMap->GetName())) iferr_return;
			}
			maxon::BaseArray<Placed> placed;
			PlaceOnSurface(surface, settings, GetShaderParameter(scatter, CYCLESSCATTER_DENSITY_SHADER, doc), fields, scatter, doc, placed, stats, warnings) iferr_return;

			// The copies' axes, then the shaders that vary them, sampled where each copy sits.
			ShaderSampler position;
			ShaderSampler scale;
			ShaderSampler rotation;
			position.Init(GetShaderParameter(scatter, CYCLESSCATTER_POSITION_SHADER, doc), doc);
			scale.Init(GetShaderParameter(scatter, CYCLESSCATTER_SCALE_SHADER, doc), doc);
			rotation.Init(GetShaderParameter(scatter, CYCLESSCATTER_ROTATION_SHADER, doc), doc);
			copies.matrices.EnsureCapacity(placed.GetCount()) iferr_return;
			for (maxon::Int i = 0; i < placed.GetCount(); ++i)
			{
				const Placed& copy = placed[i];
				cinema::Matrix m = AlignedMatrix(copy.p, copy.n, settings.up, settings.normalAlign);
				if (rotation.IsActive())
				{
					const maxon::Float64 angle = rotation.Sample(copy.uv, copy.n) * settings.rotationAngle;
					const maxon::Float64 c = maxon::Cos(angle);
					const maxon::Float64 s = maxon::Sin(angle);
					const cinema::Vector y = m.sqmat.v2 * c + m.sqmat.v3 * s;
					const cinema::Vector z = m.sqmat.v3 * c - m.sqmat.v2 * s;
					m.sqmat.v2 = y;
					m.sqmat.v3 = z;
				}
				if (position.IsActive())
					m.off += m.sqmat.v2 * (position.Sample(copy.uv, copy.n) * settings.positionDistance);
				maxon::Float64 factor = copy.field;
				if (scale.IsActive())
					factor *= settings.scaleMin + (settings.scaleMax - settings.scaleMin) * scale.Sample(copy.uv, copy.n);
				if (factor != 1.0)
					m.sqmat = m.sqmat * factor;
				copies.matrices.Append(m) iferr_return;
				copies.colors.Append(SCATTER_COPY_COLOR) iferr_return;
				uvs.Append(copy.uv) iferr_return;
				const maxon::Int32 source = sourceCount <= 0 ? 0
					: settings.childOrder == CYCLESSCATTER_CHILD_ORDER_ITERATE ? maxon::Int32(copy.index % maxon::UInt64(sourceCount))
					: maxon::Int32(maxon::Min(sourceCount - 1, maxon::Int(Draw(settings.seed, copy.index, DRAW_CHILD) * maxon::Float64(sourceCount))));
				copies.sources.Append(source) iferr_return;
			}
		}
	}
	stats.placed = copies.matrices.GetCount();
	stats.distributeMs = cinema::GeGetMilliSeconds() - started;

	const maxon::Float64 effectorsStarted = cinema::GeGetMilliSeconds();
	maxon::BaseArray<cinema::BaseObject*> effectors;
	GetEffectors(scatter, doc, effectors) iferr_return;
	RunEffectors(scatter, effectors, thread, sourceCount, uvs, copies, stats, warnings) iferr_return;
	stats.effectorMs = cinema::GeGetMilliSeconds() - effectorsStarted;
	return maxon::OK;
}

// --- what the copies depend on -----------------------------------------------------------------------

// The objects of @p op's caches and below it, by identity and dirty counts.
static maxon::UInt64 MixObjectTree(maxon::UInt64 hash, const cinema::BaseObject* op, maxon::Int depth)
{
	if (!op || depth > 64)
		return hash;
	hash = MixValue(hash, maxon::UInt64(reinterpret_cast<maxon::UInt>(op)));
	hash = MixValue(hash, op->GetDirty(cinema::DIRTYFLAGS::DATA | cinema::DIRTYFLAGS::MATRIX | cinema::DIRTYFLAGS::CACHE));
	hash = MixValue(hash, op->GetBit(BIT_CONTROLOBJECT) ? 1 : 0);
	hash = MixObjectTree(hash, op->GetDeformCache(), depth + 1);
	hash = MixObjectTree(hash, op->GetCache(), depth + 1);
	for (const cinema::BaseObject* child = op->GetDown(); child; child = child->GetNext())
		hash = MixObjectTree(hash, child, depth + 1);
	return hash;
}

static maxon::UInt64 MixShaderTree(maxon::UInt64 hash, const cinema::BaseList2D* shader, maxon::Int depth)
{
	for (; shader && depth < 64; shader = shader->GetNext())
	{
		hash = MixValue(hash, maxon::UInt64(reinterpret_cast<maxon::UInt>(shader)));
		hash = MixValue(hash, shader->GetDirty(cinema::DIRTYFLAGS::DATA));
		hash = MixShaderTree(hash, static_cast<const cinema::BaseList2D*>(shader->GetDown()), depth + 1);
	}
	return hash;
}

maxon::UInt64 ScatterDependencies(const cinema::BaseObject* scatter, const cinema::BaseDocument* doc)
{
	maxon::UInt64 hash = MixMatrix(0x5C477E7, scatter->GetMg());
	maxon::Bool timed = false;
	const cinema::BaseList2D* const link = GetParameterData(scatter, CYCLESSCATTER_SURFACE).GetLink(doc);
	if (link && link->IsInstanceOf(Obase) && link != scatter)
	{
		const cinema::BaseObject* const surface = static_cast<const cinema::BaseObject*>(link);
		hash = MixMatrix(hash, surface->GetMg());
		hash = MixObjectTree(hash, surface, 0);
	}
	// A vertex map may be animated (a field on it, point level animation): it follows the time too.
	if (const cinema::BaseList2D* const map = GetParameterData(scatter, CYCLESSCATTER_VERTEX_MAP).GetLink(doc))
	{
		hash = MixValue(hash, map->GetDirty(cinema::DIRTYFLAGS::DATA));
		timed = true;
	}
	if (GetIntParameter(scatter, CYCLESSCATTER_MODE, CYCLESSCATTER_MODE_SURFACE) == CYCLESSCATTER_MODE_FILE && GetBoolParameter(scatter, CYCLESSCATTER_FILE_SEQUENCE, false))
		timed = true;
	static const maxon::Int32 SHADERS[] = { CYCLESSCATTER_DENSITY_SHADER, CYCLESSCATTER_POSITION_SHADER, CYCLESSCATTER_SCALE_SHADER, CYCLESSCATTER_ROTATION_SHADER };
	for (const maxon::Int32 id : SHADERS)
	{
		if (const cinema::BaseShader* const shader = GetShaderParameter(scatter, id, doc))
		{
			hash = MixValue(hash, maxon::UInt64(reinterpret_cast<maxon::UInt>(shader)));
			hash = MixValue(hash, shader->GetDirty(cinema::DIRTYFLAGS::DATA));
			hash = MixShaderTree(hash, static_cast<const cinema::BaseList2D*>(static_cast<const cinema::BaseList2D*>(shader)->GetDown()), 0);
			timed = true;
		}
	}
	const cinema::GeData effectorData = GetParameterData(scatter, CYCLESSCATTER_EFFECTORS);
	if (const cinema::InExcludeData* const list = effectorData.GetCustomDataType<cinema::InExcludeData>())
	{
		for (maxon::Int32 i = 0; i < list->GetObjectCount(); ++i)
		{
			const cinema::BaseList2D* const entry = list->ObjectFromIndex(doc, i);
			if (!entry || !entry->IsInstanceOf(Obase))
				continue;
			const cinema::BaseObject* const effector = static_cast<const cinema::BaseObject*>(entry);
			hash = MixValue(hash, maxon::UInt64(reinterpret_cast<maxon::UInt>(effector)));
			hash = MixValue(hash, effector->GetDirty(cinema::DIRTYFLAGS::DATA | cinema::DIRTYFLAGS::MATRIX));
			hash = MixValue(hash, effector->GetDeformMode() ? 1 : 0);
			hash = MixMatrix(hash, effector->GetMg());
			// An effector's Fields list (its Falloff tab, ofalloff_panel.h).
			const cinema::GeData fieldData = GetParameterData(effector, FIELDS);
			if (const cinema::FieldList* const fields = fieldData.GetCustomDataType<cinema::FieldList>())
				hash = MixValue(hash, fields->GetDirty(doc));
			timed = true;
		}
	}
	cinema::GeData fieldHolder;
	if (const cinema::FieldList* const fields = GetFieldList(scatter, fieldHolder))
	{
		hash = MixValue(hash, fields->GetDirty(doc));
		timed = true;
	}
	// Effectors, fields and shaders may change with the time; a plain distribution does not.
	if (timed && doc)
		hash = MixFloat(hash, doc->GetTime().Get());
	return hash;
}

} // namespace cyclesrender
