// SPDX-License-Identifier: Apache-2.0
// Hair as a `curves` item and particles as a `pointcloud` item (curves_export.h).

#include "c4d.h"
#include "customgui_gradient.h"
#include "lib_hair.h"
#include "lib_particlegroupobject.h"
#include "mhairmaterial.h"
#include "thairmaterial.h"

#include "../materials/node_graph.h"
#include "curves_export.h"
#include "parameters.h"

namespace cyclesrender
{

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

maxon::Result<maxon::Bool> ExportHair(SceneWriter& writer, const Coords& coords, cinema::BaseDocument* doc, cinema::BaseObject* op,
	const maxon::String& id)
{
	iferr_scope;

	cinema::HairObject* const hair = static_cast<cinema::HairObject*>(op);
	const maxon::Bool locked = hair->Lock(doc, nullptr, false, 0);
	cinema::HairGuides* guides = hair->GenerateHair(HAIR_GENERATE_FLAGS_NO_MATERIAL);
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

	// The hair material: thickness at root and tip (a diameter), and the colour gradient.
	const cinema::BaseTag* const materialTag = op->GetTag(Thairmaterial);
	const cinema::BaseList2D* const material = materialTag ? GetParameterData(materialTag, HAIRMATERIAL_TAG_LINK).GetLink(doc) : nullptr;
	const maxon::Float64 rootRadius = coords.Length(GetFloatParameter(material, HAIRMATERIAL_THICKNESS_ROOT, 0.5)) * 0.5;
	const maxon::Float64 tipRadius = coords.Length(GetFloatParameter(material, HAIRMATERIAL_THICKNESS_TIP, 0.03)) * 0.5;
	cinema::Vector rootColor(0.2, 0.12, 0.06);
	cinema::Vector tipColor(0.4, 0.26, 0.13);
	if (material)
	{
		const cinema::GeData gradientData = GetParameterData(material, HAIRMATERIAL_COLOR_GRADIENT);
		const cinema::Gradient* const gradient = static_cast<const cinema::Gradient*>(gradientData.GetCustomDataTypeI(CUSTOMDATATYPE_GRADIENT));
		const maxon::Int32 knots = gradient ? gradient->GetKnotCount() : 0;
		for (maxon::Int32 i = 0; i < knots; ++i)
		{
			const maxon::GradientKnot knot = gradient->GetKnot(i);
			const cinema::Vector color(knot.col.r * knot.brightness, knot.col.g * knot.brightness, knot.col.b * knot.brightness);
			// The knots are not sorted: take the ones nearest to the two ends.
			if (i == 0 || knot.pos <= gradient->GetKnot(0).pos)
				rootColor = color;
			if (i == 0 || knot.pos >= gradient->GetKnot(0).pos)
				tipColor = color;
		}
	}

	// The material.
	NodeGraph graph;
	const maxon::Int info = graph.Add("ShaderNodeHairInfo") iferr_return;
	const maxon::Int ramp = graph.Add("ShaderNodeValToRGB") iferr_return;
	const maxon::Int shader = graph.Add("ShaderNodeBsdfHairPrincipled") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	JsonWriter stops;
	stops.BeginObject();
	stops.String("interpolation", "LINEAR"_s);
	stops.BeginArray("elements");
	const maxon::Float64 root[5] = { 0.0, rootColor.x, rootColor.y, rootColor.z, 1.0 };
	const maxon::Float64 tip[5] = { 1.0, tipColor.x, tipColor.y, tipColor.z, 1.0 };
	stops.Floats(nullptr, root, 5);
	stops.Floats(nullptr, tip, 5);
	stops.EndArray();
	stops.EndObject();
	graph.SetPropertyJson(ramp, "color_ramp", stops.GetText());
	graph.Link(NodeOutput { info, "Intercept"_s }, ramp, "Fac") iferr_return;
	graph.Link(NodeOutput { ramp, "Color"_s }, shader, "Color") iferr_return;
	graph.Link(NodeOutput { shader, "BSDF"_s }, output, "Surface") iferr_return;
	const maxon::String materialId = id + ".material"_s;
	{
		JsonWriter& json = writer.BeginItem("material", materialId);
		graph.Write(json);
		writer.EndItem();
	}

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
