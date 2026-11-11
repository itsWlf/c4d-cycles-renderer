// SPDX-License-Identifier: Apache-2.0
// One polygon object as a `mesh` item and a `points` item (mesh_export.h).

#include "c4d.h"
#include "tphong.h"

#include "mesh_export.h"
#include "parameters.h"

namespace cyclesrender
{

// The axis swap mirrors the mesh, so each face is sent with its corners in reverse order to
// keep its front side. Corner 0 stays first. Every per-corner array uses the same order.
static const maxon::Int32 QUAD_ORDER[4] = { 0, 3, 2, 1 };
static const maxon::Int32 TRIANGLE_ORDER[3] = { 0, 2, 1 };

static inline const maxon::Int32* CornerOrder(const cinema::CPolygon& polygon, maxon::Int32& count)
{
	if (polygon.IsTriangle())
	{
		count = 3;
		return TRIANGLE_ORDER;
	}
	count = 4;
	return QUAD_ORDER;
}

// Frees the array CreatePhongNormals() hands out when the export leaves, on every path.
class PhongNormals
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(PhongNormals);

public:
	explicit PhongNormals(const cinema::PolygonObject* op) : _normals(op->CreatePhongNormals()) { }
	~PhongNormals() { DeleteMem(_normals); }
	const cinema::Vector32* Get() const { return _normals; }

private:
	cinema::Vector32* _normals;
};

// A named array of the mesh item: {"name": ..., "domain": ..., "data": blob}.
struct NamedBlob
{
	maxon::String name;
	maxon::Int blob = NOTOK;
	maxon::Bool perCorner = false;
};

// Blender keeps UV sets, colours and other attributes of a mesh under one set of names.
static maxon::Bool IsNameFree(const maxon::String& name, const maxon::BaseArray<NamedBlob>& a, const maxon::BaseArray<NamedBlob>& b,
	const maxon::BaseArray<NamedBlob>& c)
{
	for (const maxon::BaseArray<NamedBlob>* list : { &a, &b, &c })
	{
		for (const NamedBlob& entry : *list)
		{
			if (entry.name == name)
				return false;
		}
	}
	return true;
}

static void WriteNamedBlobs(JsonWriter& json, const maxon::Char* field, const maxon::BaseArray<NamedBlob>& entries, maxon::Bool withDomain)
{
	if (entries.IsEmpty())
		return;
	json.BeginArray(field);
	for (const NamedBlob& entry : entries)
	{
		json.BeginObject();
		json.String("name", entry.name);
		if (withDomain)
			json.String("domain", entry.perCorner ? "CORNER"_s : "POINT"_s);
		json.Int("data", entry.blob);
		json.EndObject();
	}
	json.EndArray();
}

maxon::Result<maxon::Bool> ExportMesh(SceneWriter& writer, const Coords& coords, const cinema::PolygonObject* op, const maxon::String& meshId,
	const MeshMaterials& materials, const MeshMotion& motion)
{
	iferr_scope;

	const cinema::Vector* const points = op->GetPointR();
	const cinema::CPolygon* const polygons = op->GetPolygonR();
	const maxon::Int32 pointCount = op->GetPointCount();
	const maxon::Int32 polygonCount = op->GetPolygonCount();
	if (!points || !polygons || pointCount <= 0 || polygonCount <= 0)
		return false;

	maxon::Int cornerCount = 0;
	for (maxon::Int32 i = 0; i < polygonCount; ++i)
		cornerCount += polygons[i].IsTriangle() ? 3 : 4;

	// --- the mesh item: everything but positions and normals -------------------------------

	// Faces
	maxon::Int cornersBlob = 0;
	maxon::Int sizesBlob = 0;
	maxon::Int32* const outCorners = writer.AllocInts(cornerCount, cornersBlob) iferr_return;
	maxon::Int32* const outSizes = writer.AllocInts(polygonCount, sizesBlob) iferr_return;
	maxon::Int corner = 0;
	for (maxon::Int32 i = 0; i < polygonCount; ++i)
	{
		maxon::Int32 count = 0;
		const maxon::Int32* const order = CornerOrder(polygons[i], count);
		for (maxon::Int32 c = 0; c < count; ++c)
			outCorners[corner++] = polygons[i][order[c]];
		outSizes[i] = count;
	}

	maxon::Int slotsBlob = NOTOK;
	if (materials.polygonSlots)
	{
		maxon::Int32* const outSlots = writer.AllocInts(polygonCount, slotsBlob) iferr_return;
		maxon::MemCopyType(outSlots, materials.polygonSlots, polygonCount);
	}

	maxon::BaseArray<NamedBlob> uvSets;
	maxon::BaseArray<NamedBlob> floatMaps;
	maxon::BaseArray<NamedBlob> colorMaps;
	maxon::Int renderUv = NOTOK;
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		maxon::String name = tag->GetName();
		if (tag->IsInstanceOf(Tuvw))
		{
			// Cinema 4D's V runs down from the top of the image, Blender's up from the bottom.
			const cinema::UVWTag* const uvwTag = static_cast<const cinema::UVWTag*>(tag);
			if (uvwTag->GetDataCount() != polygonCount)
				continue;
			// Several UVW tags usually carry the same name; each still becomes a set of its own.
			if (name.IsEmpty())
				name = "UVW"_s;
			const maxon::String baseName = name;
			for (maxon::Int suffix = 1; !IsNameFree(name, uvSets, floatMaps, colorMaps); ++suffix)
				name = FormatString("@.@", baseName, suffix);

			NamedBlob entry;
			entry.name = name;
			maxon::Float32* const outUvs = writer.AllocFloats(cornerCount * 2, entry.blob) iferr_return;
			cinema::ConstUVWHandle handle = uvwTag->GetDataAddressR();
			corner = 0;
			for (maxon::Int32 i = 0; i < polygonCount; ++i)
			{
				cinema::UVWStruct uvw;
				cinema::UVWTag::Get(handle, i, uvw);
				maxon::Int32 count = 0;
				const maxon::Int32* const order = CornerOrder(polygons[i], count);
				for (maxon::Int32 c = 0; c < count; ++c, ++corner)
				{
					const cinema::Vector& uv = uvw[order[c]];
					outUvs[corner * 2] = maxon::Float32(uv.x);
					outUvs[corner * 2 + 1] = maxon::Float32(1.0 - uv.y);
				}
			}
			if (uvwTag == materials.renderUv)
				renderUv = uvSets.GetCount();
			uvSets.Append(std::move(entry)) iferr_return;
		}
		else if (tag->IsInstanceOf(Tvertexmap))
		{
			// One weight per point. A material's Vertex Map shader finds it by the tag's name; of
			// two tags with one name the first is the one found.
			const cinema::VertexMapTag* const mapTag = static_cast<const cinema::VertexMapTag*>(tag);
			const maxon::Float32* const weights = mapTag->GetDataAddressR();
			if (!weights || mapTag->GetDataCount() != pointCount || name.IsEmpty() || !IsNameFree(name, uvSets, floatMaps, colorMaps))
				continue;
			NamedBlob entry;
			entry.name = name;
			maxon::Float32* const out = writer.AllocFloats(pointCount, entry.blob) iferr_return;
			maxon::MemCopyType(out, weights, pointCount);
			floatMaps.Append(std::move(entry)) iferr_return;
		}
		else if (tag->IsInstanceOf(Tvertexcolor))
		{
			const cinema::VertexColorTag* const colorTag = static_cast<const cinema::VertexColorTag*>(tag);
			cinema::ConstVertexColorHandle handle = colorTag->GetDataAddressR();
			const maxon::Bool perPoint = colorTag->IsPerPointColor();
			if (!handle || colorTag->GetDataCount() != (perPoint ? pointCount : polygonCount) || name.IsEmpty() || !IsNameFree(name, uvSets, floatMaps, colorMaps))
				continue;
			NamedBlob entry;
			entry.name = name;
			entry.perCorner = !perPoint;
			if (perPoint)
			{
				maxon::Float32* const out = writer.AllocFloats(maxon::Int(pointCount) * 4, entry.blob) iferr_return;
				for (maxon::Int32 i = 0; i < pointCount; ++i)
				{
					const maxon::ColorA32 color = cinema::VertexColorTag::Get(handle, nullptr, nullptr, i);
					out[i * 4 + 0] = color.r;
					out[i * 4 + 1] = color.g;
					out[i * 4 + 2] = color.b;
					out[i * 4 + 3] = color.a;
				}
			}
			else
			{
				maxon::Float32* const out = writer.AllocFloats(cornerCount * 4, entry.blob) iferr_return;
				corner = 0;
				for (maxon::Int32 i = 0; i < polygonCount; ++i)
				{
					cinema::VertexColorStruct colors;
					cinema::VertexColorTag::Get(handle, i, colors);
					const maxon::ColorA32* const perCorner[4] = { &colors.a, &colors.b, &colors.c, &colors.d };
					maxon::Int32 count = 0;
					const maxon::Int32* const order = CornerOrder(polygons[i], count);
					for (maxon::Int32 c = 0; c < count; ++c, ++corner)
					{
						const maxon::ColorA32& color = *perCorner[order[c]];
						out[corner * 4 + 0] = color.r;
						out[corner * 4 + 1] = color.g;
						out[corner * 4 + 2] = color.b;
						out[corner * 4 + 3] = color.a;
					}
				}
			}
			colorMaps.Append(std::move(entry)) iferr_return;
		}
	}

	{
		JsonWriter& json = writer.BeginItem("mesh", meshId);
		json.Int("point_count", pointCount);
		json.Int("corner_verts", cornersBlob);
		json.Int("face_sizes", sizesBlob);
		json.Bool("smooth", false);
		WriteNamedBlobs(json, "uvs", uvSets, false);
		if (renderUv > 0)
			json.Int("uv_render", renderUv);
		WriteNamedBlobs(json, "floats", floatMaps, true);
		WriteNamedBlobs(json, "colors", colorMaps, true);
		json.Int("slots", materials.slotCount);
		if (slotsBlob != NOTOK)
			json.Int("material_indices", slotsBlob);
		writer.EndItem();
	}

	// --- the points item -------------------------------------------------------------------

	maxon::Int pointsBlob = 0;
	maxon::Float32* const outPoints = writer.AllocFloats(maxon::Int(pointCount) * 3, pointsBlob) iferr_return;
	for (maxon::Int32 i = 0; i < pointCount; ++i)
		coords.Point(points[i], outPoints + maxon::Int(i) * 3);

	// Shading normals, one per face corner
	maxon::Int normalsBlob = NOTOK;
	const cinema::NormalTag* const normalTag = static_cast<const cinema::NormalTag*>(op->GetTag(Tnormal));
	if (normalTag && normalTag->GetDataCount() == polygonCount)
	{
		maxon::Float32* const outNormals = writer.AllocFloats(cornerCount * 3, normalsBlob) iferr_return;
		cinema::ConstNormalHandle handle = normalTag->GetDataAddressR();
		corner = 0;
		for (maxon::Int32 i = 0; i < polygonCount; ++i)
		{
			cinema::NormalStruct normals;
			cinema::NormalTag::Get(handle, i, normals);
			const cinema::Vector* const perCorner[4] = { &normals.a, &normals.b, &normals.c, &normals.d };
			maxon::Int32 count = 0;
			const maxon::Int32* const order = CornerOrder(polygons[i], count);
			for (maxon::Int32 c = 0; c < count; ++c, ++corner)
				Coords::Direction(cinema::Vector32(*perCorner[order[c]]), outNormals + corner * 3);
		}
	}
	else
	{
		const PhongNormals phong(op);
		if (phong.Get())
		{
			maxon::Float32* const outNormals = writer.AllocFloats(cornerCount * 3, normalsBlob) iferr_return;
			corner = 0;
			for (maxon::Int32 i = 0; i < polygonCount; ++i)
			{
				maxon::Int32 count = 0;
				const maxon::Int32* const order = CornerOrder(polygons[i], count);
				for (maxon::Int32 c = 0; c < count; ++c, ++corner)
					Coords::Direction(phong.Get()[maxon::Int(i) * 4 + order[c]], outNormals + corner * 3);
			}
		}
	}

	// Motion blur: how far each point goes from shutter open to shutter close.
	maxon::Int motionBlob = NOTOK;
	if (motion.open && motion.close && motion.count == pointCount)
	{
		maxon::Bool moves = false;
		for (maxon::Int32 i = 0; i < pointCount && !moves; ++i)
			moves = motion.open[i] != motion.close[i];
		if (moves)
		{
			maxon::Float32* const outMotion = writer.AllocFloats(maxon::Int(pointCount) * 3, motionBlob) iferr_return;
			for (maxon::Int32 i = 0; i < pointCount; ++i)
				coords.Point(cinema::Vector(motion.close[i] - motion.open[i]), outMotion + maxon::Int(i) * 3);
		}
	}

	JsonWriter& json = writer.BeginItem("points", meshId + ".points"_s);
	json.String("mesh", meshId);
	json.Int("points", pointsBlob);
	if (motionBlob != NOTOK)
		json.Int("motion", motionBlob);
	if (normalsBlob != NOTOK)
		json.Int("normals", normalsBlob);
	// When the mesh is built again its points are gone, so they must follow it.
	writer.SendWith(meshId);
	writer.EndItem();
	return true;
}

maxon::UInt64 HashMeshSource(const cinema::PolygonObject* op)
{
	const maxon::Int32 pointCount = op->GetPointCount();
	const maxon::Int32 polygonCount = op->GetPolygonCount();
	maxon::UInt64 hash = maxon::UInt64(pointCount) * 0x9E3779B97F4A7C15ull + maxon::UInt64(polygonCount);
	if (const cinema::Vector* const points = op->GetPointR())
		hash = HashBytes(reinterpret_cast<const maxon::Char*>(points), maxon::Int(pointCount) * SIZEOF(cinema::Vector), hash);
	if (const cinema::CPolygon* const polygons = op->GetPolygonR())
		hash = HashBytes(reinterpret_cast<const maxon::Char*>(polygons), maxon::Int(polygonCount) * SIZEOF(cinema::CPolygon), hash);

	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
	{
		const maxon::Int32 type = tag->GetType();
		if (type == Tphong)
		{
			// The Phong tag decides the normals.
			hash = hash * 31 + maxon::UInt64(GetBoolParameter(tag, PHONGTAG_PHONG_ANGLELIMIT, false));
			hash = hash * 31 + maxon::UInt64(GetFloatParameter(tag, PHONGTAG_PHONG_ANGLE, 0.0) * 1e6);
			hash = hash * 31 + maxon::UInt64(GetBoolParameter(tag, PHONGTAG_PHONG_USEEDGES, false));
		}
		else if (type == Tuvw || type == Tnormal || type == Tvertexmap || type == Tvertexcolor)
		{
			const cinema::VariableTag* const variable = static_cast<const cinema::VariableTag*>(tag);
			hash = hash * 31 + maxon::UInt64(type) + maxon::UInt64(tag->GetName().GetHashCode());
			if (const void* const data = variable->GetLowlevelDataAddressR())
				hash = HashBytes(static_cast<const maxon::Char*>(data), maxon::Int(variable->GetDataCount()) * variable->GetDataSize(), hash);
		}
		else if (type == Tpolygonselection)
		{
			hash = hash * 31 + maxon::UInt64(tag->GetName().GetHashCode());
			const cinema::BaseSelect* const selection = static_cast<const cinema::SelectionTag*>(tag)->GetBaseSelect();
			for (maxon::Int32 segment = 0; selection && segment < selection->GetSegments(); ++segment)
			{
				maxon::Int32 from = 0;
				maxon::Int32 to = 0;
				if (selection->GetRange(segment, polygonCount, &from, &to))
					hash = (hash ^ (maxon::UInt64(from) << 32 | maxon::UInt64(to))) * 0x100000001B3ull;
			}
		}
	}
	return hash;
}

} // namespace cyclesrender
