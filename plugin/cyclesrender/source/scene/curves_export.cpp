// SPDX-License-Identifier: Apache-2.0
// Hair as a `curves` item and particles as a `pointcloud` item (curves_export.h).

#include "c4d.h"
#include "customgui_gradient.h"
#include "lib_hair.h"
#include "lib_particlegroupobject.h"
#include "mhairmaterial.h"
#include "ohair.h"
#include "thairmaterial.h"

#include "../materials/node_graph.h"
#include "curves_export.h"
#include "document_colors.h"
#include "parameters.h"

namespace cyclesrender
{

// The entry of a Hair object's container in which Cinema 4D counts its evaluations (found by comparing
// the container before and after one; it has no name in the SDK). HairSignature().
static const maxon::Int32 HAIR_EVALUATION_COUNT = 50004;

// Radius of a particle whose group stores none, in document units.
static const maxon::Float64 DEFAULT_PARTICLE_RADIUS = 1.0;

static void WriteMaterialList(JsonWriter& json, const maxon::String& materialId)
{
	json.BeginArray("materials");
	json.String(nullptr, materialId);
	json.EndArray();
}

// The identity placement: the points are sent in world space.
static void WriteWorldMatrix(JsonWriter& json, const Coords& coords)
{
	maxon::Float64 matrix[16];
	coords.ObjectMatrix(cinema::Matrix(), matrix);
	json.Floats("matrix", matrix, 16);
}

// Everything a Hair object's `curves` item is made from, as one number (ExportHair, @p reuse).
//
// Not the Hair object's own change counters: every evaluation of the document moves them (measured in
// Cinema 4D 2027, internal/f2/probe_dirty.py: its data and cache counts and its hierarchy count went up
// at each ExecutePasses with nothing changed, while generating the hairs moved none), so with them a
// hair would never be kept. What it is made from is read instead: the object's settings (its container's
// own count, less what an evaluation adds to it: below), its guides' points (and the guides dynamics moved, when it has them), its tags, its
// place; the hair material; the object it grows on with its tags, its cache (which a deformer or a
// generator builds anew) and its place; the document's unit; the thickness, and the ids of the item
// and its material.
static maxon::UInt64 HairSignature(cinema::BaseDocument* doc, cinema::BaseObject* op, const cinema::BaseList2D* material, const Coords& coords,
	const maxon::String& id, const maxon::String& materialId, maxon::Float64 rootRadius, maxon::Float64 tipRadius)
{
	maxon::UInt64 hash = maxon::UInt64(id.GetHashCode()) * 0x9E3779B97F4A7C15ull + maxon::UInt64(materialId.GetHashCode());
	const auto mix = [&hash](const void* data, maxon::Int size)
	{
		hash = HashBytes(static_cast<const maxon::Char*>(data), size, hash);
	};
	const auto mixNumber = [&mix](maxon::UInt32 value)
	{
		mix(&value, SIZEOF(value));
	};
	const auto mixPlaceAndTags = [&mix, &mixNumber](cinema::BaseObject* object)
	{
		const cinema::Matrix mg = object->GetMg();
		mix(&mg, SIZEOF(mg));
		for (cinema::BaseTag* tag = object->GetFirstTag(); tag; tag = tag->GetNext())
		{
			mixNumber(maxon::UInt32(tag->GetType()));
			mixNumber(tag->GetDirty(cinema::DIRTYFLAGS::DATA));
		}
	};
	const auto mixGuides = [&mix, &mixNumber](cinema::HairGuides* guides)
	{
		if (!guides)
			return;
		const maxon::Int32 count = guides->GetPointCount();
		mixNumber(maxon::UInt32(count));
		mixNumber(maxon::UInt32(guides->GetGuidePointCount()));
		if (const cinema::Vector* const points = guides->GetPoints())
			mix(points, maxon::Int(count) * SIZEOF(cinema::Vector));
	};
	cinema::HairObject* const hair = static_cast<cinema::HairObject*>(op);
	// The settings: the container's count goes up with every value set. An evaluation of the document sets
	// one too, a count of its own under HAIR_EVALUATION_COUNT that goes up by one each time while the
	// container's count goes up by two (measured, Cinema 4D 2027, internal/f2/probe_container.py: no
	// other plain value of the container changed). Taken off, what is left moves only when a setting does.
	// (Where Cinema 4D counts differently the number moves with every evaluation and the hairs are
	// generated every time, as before 0.3.0: never kept wrongly.)
	const cinema::BaseContainer& data = op->GetDataInstanceRef();
	mixNumber(data.GetDirty() - 2 * maxon::UInt32(data.GetInt32(HAIR_EVALUATION_COUNT)));
	mixGuides(hair->GetGuides());
	mixGuides(hair->GetDynamicGuides());
	mixPlaceAndTags(op);
	if (cinema::BaseObject* const root = static_cast<cinema::BaseObject*>(GetParameterData(op, HAIRSTYLE_LINK).GetLink(doc, Obase)))
	{
		mixNumber(root->GetDirty(cinema::DIRTYFLAGS::DATA));
		mixNumber(root->GetDirty(cinema::DIRTYFLAGS::CACHE));
		mixPlaceAndTags(root);
	}
	mixNumber(material ? material->GetDirty(cinema::DIRTYFLAGS::ALL) : 0);
	// (Not the document's time: what moves hairs over time is in the numbers above, an animated setting
	// or material by its count, a moving or deforming root by its place and cache, dynamics by the guides
	// they moved. A hair material channel driven by a shader that changes with time and has no count is not
	// seen: such hairs keep their shape until something else of them changes or the scene is sent anew.)
	const maxon::Float64 numbers[3] = { coords.GetScale(), rootRadius, tipRadius };
	mix(numbers, SIZEOF(numbers));
	return hash;
}

maxon::Result<maxon::Bool> ExportHair(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, cinema::BaseObject* op,
	const maxon::String& id, const maxon::String& ownMaterialId, const SyncState* reuse, maxon::Bool note, maxon::Bool* kept)
{
	iferr_scope;
	if (kept)
		*kept = false;

	// The hair material: thickness at root and tip (a diameter), and the colour gradient.
	const cinema::BaseTag* const materialTag = op->GetTag(Thairmaterial);
	const cinema::BaseList2D* const material = materialTag ? GetParameterData(materialTag, HAIRMATERIAL_TAG_LINK).GetLink(doc) : nullptr;
	const maxon::Float64 rootRadius = coords.Length(GetFloatParameter(material, HAIRMATERIAL_THICKNESS_ROOT, 0.5)) * 0.5;
	const maxon::Float64 tipRadius = coords.Length(GetFloatParameter(material, HAIRMATERIAL_THICKNESS_TIP, 0.03)) * 0.5;

	maxon::String materialId = ownMaterialId;
	if (materialId.IsEmpty())
	{
		// The hair material's own colours: its gradient from root to tip, every knot.
		struct Stop
		{
			maxon::Float64 position;
			cinema::Vector color;
		};
		maxon::BaseArray<Stop> stops;
		if (material)
		{
			writer.Warn(FormatString("Hair \"@\": of its hair material \"@\" the shape, the thickness and the colour gradient are translated; "
				"its other shading channels (specular, transparency, backlit and the rest) are not.", op->GetName(), material->GetName()));
			const cinema::GeData gradientData = GetParameterData(material, HAIRMATERIAL_COLOR_GRADIENT);
			const cinema::Gradient* const gradient = static_cast<const cinema::Gradient*>(gradientData.GetCustomDataTypeI(CUSTOMDATATYPE_GRADIENT));
			const maxon::Int32 knots = gradient ? gradient->GetKnotCount() : 0;
			const DocumentColors colors(doc);
			for (maxon::Int32 i = 0; i < knots; ++i)
			{
				const maxon::GradientKnot knot = gradient->GetKnot(i);
				// A knot is a colour parameter (DocumentColors::Input; measured for the Gradient shader's).
				Stop stop;
				stop.position = maxon::Clamp01(maxon::Float64(knot.pos));
				stop.color = colors.Input(cinema::Vector(knot.col.r, knot.col.g, knot.col.b) * knot.brightness);
				// The knots are not sorted: each goes before the first one that lies further along.
				maxon::Int at = 0;
				while (at < stops.GetCount() && stops[at].position <= stop.position)
					++at;
				stops.Insert(at, stop) iferr_return;
			}
		}
		if (stops.IsEmpty())
		{
			stops.Append(Stop { 0.0, cinema::Vector(0.2, 0.12, 0.06) }) iferr_return;
			stops.Append(Stop { 1.0, cinema::Vector(0.4, 0.26, 0.13) }) iferr_return;
		}

		NodeGraph graph;
		const maxon::Int info = graph.Add("ShaderNodeHairInfo") iferr_return;
		const maxon::Int ramp = graph.Add("ShaderNodeValToRGB") iferr_return;
		const maxon::Int shader = graph.Add("ShaderNodeBsdfHairPrincipled") iferr_return;
		const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
		JsonWriter elements;
		elements.BeginObject();
		elements.String("interpolation", "LINEAR"_s);
		elements.BeginArray("elements");
		for (const Stop& stop : stops)
		{
			const maxon::Float64 element[5] = { stop.position, stop.color.x, stop.color.y, stop.color.z, 1.0 };
			elements.Floats(nullptr, element, 5);
		}
		elements.EndArray();
		elements.EndObject();
		graph.SetPropertyJson(ramp, "color_ramp", elements.GetText());
		graph.Link(NodeOutput { info, "Intercept"_s }, ramp, "Fac") iferr_return;
		graph.Link(NodeOutput { ramp, "Color"_s }, shader, "Color") iferr_return;
		graph.Link(NodeOutput { shader, "BSDF"_s }, output, "Surface") iferr_return;
		materialId = id + ".material"_s;
		JsonWriter& json = writer.BeginItem("material", materialId);
		graph.Write(json);
		writer.EndItem();
	}

	// Left as Blender holds it when nothing it is made from has changed: generating the hairs is the cost.
	const maxon::UInt64 signature = (reuse || note) ? HairSignature(doc, op, material, coords, id, materialId, rootRadius, tipRadius) : 0;
	if (reuse && writer.KeepItem(*reuse, id, signature))
	{
		if (kept)
			*kept = true;
		return true;
	}

	// The hairs as Cinema 4D renders them: with the hair material's shape (bend, curl, frizz, kink and the
	// rest change the points; measured, tests/c4d/hair.py).
	cinema::HairObject* const hair = static_cast<cinema::HairObject*>(op);
	const maxon::Bool locked = hair->Lock(doc, nullptr, false, 0);
	cinema::HairGuides* guides = hair->GenerateHair(HAIR_GENERATE_FLAGS_NONE);
	if (locked)
		hair->Unlock();
	finally
	{
		if (guides)
			cinema::HairGuides::Free(guides);
	};
	if (!guides)
		return false;

	const maxon::Int32 count = guides->GetCount();
	const maxon::Int32 perCurve = guides->GetGuidePointCount();
	const cinema::Vector* const points = guides->GetPoints();
	if (count < 1 || perCurve < 2 || !points)
		return false;

	// The data of the curves item. Blobs belong to the item that is ended next, so they are made
	// only now, after the material item (made before it, they went to the material, and the
	// curves read some other item's data as soon as the scene held more than hair).
	// The generated hairs are in world space already (measured: applying the guides' matrix
	// moved them away from their roots by the root object's offset).
	const maxon::Int total = maxon::Int(count) * perCurve;
	maxon::Int pointsBlob = 0;
	maxon::Float32* const outPoints = writer.AllocFloats(total * 3, pointsBlob) iferr_return;
	maxon::Int radiiBlob = 0;
	maxon::Float32* const outRadii = writer.AllocFloats(total, radiiBlob) iferr_return;
	for (maxon::Int i = 0; i < total; ++i)
	{
		coords.Point(points[i], outPoints + i * 3);
		const maxon::Float64 along = maxon::Float64(i % perCurve) / maxon::Float64(perCurve - 1);
		outRadii[i] = maxon::Float32(rootRadius + (tipRadius - rootRadius) * along);
	}

	JsonWriter& json = writer.BeginItem("curves", id);
	WriteWorldMatrix(json, coords);
	json.Int("points_per_curve", perCurve);
	json.Int("points", pointsBlob);
	json.Int("radii", radiiBlob);
	WriteMaterialList(json, materialId);
	writer.EndItem();
	if (note)
		writer.NoteItemSource(id, signature);
	return true;
}

maxon::Result<maxon::Bool> ExportParticles(SceneWriter& writer, const Coords& coords, const cinema::BaseObject* op, const cinema::Matrix& mg,
	const maxon::String& id, const maxon::String& materialId)
{
	iferr_scope;

	const cinema::ParticleGroupObject* const group = static_cast<const cinema::ParticleGroupObject*>(op);
	// A particle group is a point object; its points are the particles' positions.
	maxon::Block<const cinema::Vector> positions = group->GetParticlePositionsR();
	if (positions.IsEmpty() && group->GetPointR() && group->GetPointCount() > 0)
		positions = maxon::Block<const cinema::Vector>(group->GetPointR(), group->GetPointCount());
	const maxon::Int count = positions.GetCount();
	if (count < 1)
		return false;
	const maxon::Block<const maxon::Float32> radii = group->GetParticleRadiiR();
	const maxon::Block<const maxon::ColorA32> colors = group->GetParticleColorsR();

	const maxon::Bool hasColors = colors.GetCount() == count;

	maxon::String usedMaterial = materialId;
	if (usedMaterial.IsEmpty())
	{
		// Without a material of its own a group shows its particles' colours.
		NodeGraph graph;
		const maxon::Int shader = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
		const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
		if (hasColors)
		{
			const maxon::Int attribute = graph.Add("ShaderNodeAttribute") iferr_return;
			graph.SetProperty(attribute, "attribute_name", "particle_color"_s);
			graph.Link(NodeOutput { attribute, "Color"_s }, shader, "Base Color") iferr_return;
		}
		else
		{
			graph.SetInput(shader, "Base Color", cinema::Vector(0.8));
		}
		graph.Link(NodeOutput { shader, "BSDF"_s }, output, "Surface") iferr_return;
		usedMaterial = id + ".material"_s;
		JsonWriter& json = writer.BeginItem("material", usedMaterial);
		graph.Write(json);
		writer.EndItem();
	}

	// The data of the point cloud item, made after the material item (see ExportHair).
	maxon::Int pointsBlob = 0;
	maxon::Float32* const outPoints = writer.AllocFloats(count * 3, pointsBlob) iferr_return;
	maxon::Int radiiBlob = 0;
	maxon::Float32* const outRadii = writer.AllocFloats(count, radiiBlob) iferr_return;
	const maxon::Bool hasRadii = radii.GetCount() == count;
	for (maxon::Int i = 0; i < count; ++i)
	{
		coords.Point(mg * positions[i], outPoints + i * 3);
		outRadii[i] = maxon::Float32(coords.Length(hasRadii ? maxon::Float64(radii[i]) : DEFAULT_PARTICLE_RADIUS));
	}
	maxon::Int colorsBlob = NOTOK;
	if (hasColors)
	{
		maxon::Float32* const outColors = writer.AllocFloats(count * 4, colorsBlob) iferr_return;
		for (maxon::Int i = 0; i < count; ++i)
		{
			outColors[i * 4 + 0] = colors[i].r;
			outColors[i * 4 + 1] = colors[i].g;
			outColors[i * 4 + 2] = colors[i].b;
			outColors[i * 4 + 3] = colors[i].a;
		}
	}

	JsonWriter& json = writer.BeginItem("pointcloud", id);
	WriteWorldMatrix(json, coords);
	json.Int("points", pointsBlob);
	json.Int("radii", radiiBlob);
	if (colorsBlob != NOTOK)
		json.Int("colors", colorsBlob);
	WriteMaterialList(json, usedMaterial);
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
