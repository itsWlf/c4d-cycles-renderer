// SPDX-License-Identifier: Apache-2.0
// The `world` item: a Sky object's panorama, a Redshift dome light, or the plain default
// world of the Render Settings (environment_export.h).

#include "c4d.h"
#include "maxon/url.h"
#include "drsfile.h"
#include "mmaterial.h"
#include "orslight.h"

#include "environment_export.h"
#include "parameters.h"

namespace cyclesrender
{

// Euler angles (X, then Y, then Z: Blender's default order) of a rotation given by its
// columns x, y, z.
static cinema::Vector EulerXYZ(const cinema::Vector& x, const cinema::Vector& y, const cinema::Vector& z)
{
	// Row 2 of the matrix is (x.z, y.z, z.z) = (-sin b, sin a cos b, cos a cos b).
	const maxon::Float64 sinB = maxon::ClampValue(-x.z, -1.0, 1.0);
	const maxon::Float64 b = maxon::ASin(sinB);
	if (maxon::Abs(sinB) > 0.999999)
	{
		// Looking straight along the pole: only the sum of the other two angles is defined.
		return cinema::Vector(maxon::ATan2(-z.y, y.y), b, 0.0);
	}
	return cinema::Vector(maxon::ATan2(y.z, z.z), b, maxon::ATan2(x.y, x.x));
}

// A plain colour as the whole environment.
static maxon::Result<void> WritePlainWorld(SceneWriter& writer, const cinema::Vector& color, maxon::Float64 strength)
{
	iferr_scope;

	NodeGraph graph;
	const maxon::Int background = graph.Add("ShaderNodeBackground") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputWorld") iferr_return;
	graph.SetInput(background, "Color", color);
	graph.SetInput(background, "Strength", strength);
	graph.Link(NodeOutput { background, "Background"_s }, output, "Surface") iferr_return;

	JsonWriter& json = writer.BeginItem("world", "world"_s);
	graph.Write(json);
	writer.EndItem();
	return maxon::OK;
}

// A panorama image as the environment, turned with the object that carries it.
// @param[in] tint    Multiplies the image (a strength and a colour).
// @param[in] mirror  True for the other handedness: the image runs the other way round.
static maxon::Result<void> WritePanoramaWorld(SceneWriter& writer, const maxon::String& image, const cinema::Matrix& skyMg, const cinema::Vector& tint, maxon::Bool mirror)
{
	iferr_scope;

	// A panorama. Seen from inside an unrotated Sky object, Cinema 4D puts the image's left edge
	// towards +X and its quarter point towards +Z, continuing towards -X. Blender puts the image
	// centre towards its +X and continues towards -Y. In Blender's axes the two differ by a mirror
	// of X, which the Mapping node applies as a scale of -1.
	//
	// With R the Sky object's rotation in Blender's axes and K that mirror, a view direction d
	// must look up the image at K * R^-1 * d. The Mapping node computes Rotation * Scale * d with
	// Scale = K, so Rotation = K * R^-1 * K.
	const cinema::Vector rx = cinema::Vector(skyMg.sqmat.v1.x, skyMg.sqmat.v1.z, skyMg.sqmat.v1.y).GetNormalized();
	const cinema::Vector ry = cinema::Vector(skyMg.sqmat.v3.x, skyMg.sqmat.v3.z, skyMg.sqmat.v3.y).GetNormalized();
	const cinema::Vector rz = cinema::Vector(skyMg.sqmat.v2.x, skyMg.sqmat.v2.z, skyMg.sqmat.v2.y).GetNormalized();
	// Columns of R^-1 are the rows of R; K negates the first row and the first column.
	// (Without the mirror, for a Redshift dome map, K is the identity and Rotation = R^-1.)
	const maxon::Float64 k = mirror ? 1.0 : -1.0;
	const cinema::Vector column0(rx.x, k * ry.x, k * rz.x);
	const cinema::Vector column1(k * rx.y, ry.y, rz.y);
	const cinema::Vector column2(k * rx.z, ry.z, rz.z);
	const cinema::Vector rotation = EulerXYZ(column0, column1, column2);

	NodeGraph graph;
	const maxon::Int coordinates = graph.Add("ShaderNodeTexCoord") iferr_return;
	const maxon::Int mapping = graph.Add("ShaderNodeMapping") iferr_return;
	const maxon::Int environment = graph.Add("ShaderNodeTexEnvironment") iferr_return;
	const maxon::Int background = graph.Add("ShaderNodeBackground") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputWorld") iferr_return;
	graph.SetProperty(mapping, "vector_type", "POINT"_s);
	graph.SetInput(mapping, "Scale", cinema::Vector(mirror ? 1.0 : -1.0, 1.0, 1.0));
	graph.SetInput(mapping, "Rotation", rotation);
	graph.SetProperty(environment, "image", image);
	graph.SetProperty(environment, "projection", "EQUIRECTANGULAR"_s);
	const maxon::Float64 strength = maxon::Max(tint.x, maxon::Max(tint.y, tint.z));
	graph.SetInput(background, "Strength", strength);
	graph.Link(NodeOutput { coordinates, "Generated"_s }, mapping, "Vector") iferr_return;
	graph.Link(NodeOutput { mapping, "Vector"_s }, environment, "Vector") iferr_return;
	if (strength > 0.0 && tint != cinema::Vector(strength))
	{
		// A coloured tint: the image times the colour.
		const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
		graph.SetProperty(mix, "data_type", "RGBA"_s);
		graph.SetProperty(mix, "blend_type", "MULTIPLY"_s);
		graph.SetInput(mix, "Factor_Float", 1.0);
		graph.SetInput(mix, "B_Color", tint / strength);
		graph.Link(NodeOutput { environment, "Color"_s }, mix, "A_Color") iferr_return;
		graph.Link(NodeOutput { mix, "Result_Color"_s }, background, "Color") iferr_return;
	}
	else
	{
		graph.Link(NodeOutput { environment, "Color"_s }, background, "Color") iferr_return;
	}
	graph.Link(NodeOutput { background, "Background"_s }, output, "Surface") iferr_return;

	JsonWriter& json = writer.BeginItem("world", "world"_s);
	graph.Write(json);
	writer.EndItem();
	return maxon::OK;
}

maxon::Result<void> ExportWorld(MaterialExporter& materials, const cinema::BaseObject* sky, const cinema::Matrix& skyMg,
	cinema::BaseMaterial* skyMaterial, const cinema::Vector& defaultColor, maxon::Float64 defaultStrength)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	if (!sky)
		return WritePlainWorld(writer, defaultColor, defaultStrength);
	if (!skyMaterial || skyMaterial->GetType() != Mmaterial)
	{
		// Cinema 4D renders a Sky without a material as plain white.
		if (skyMaterial)
			writer.Warn(FormatString("Sky \"@\": only classic materials are translated for the sky so far; a white sky is used.", sky->GetName()));
		return WritePlainWorld(writer, cinema::Vector(1.0), 1.0);
	}

	// The sky shows the material's luminance channel, or its colour channel when luminance is off.
	const maxon::Bool luminance = GetBoolParameter(skyMaterial, MATERIAL_USE_LUMINANCE, false);
	const cinema::Vector color = luminance
		? GetVectorParameter(skyMaterial, MATERIAL_LUMINANCE_COLOR, cinema::Vector(1.0)) * GetFloatParameter(skyMaterial, MATERIAL_LUMINANCE_BRIGHTNESS, 1.0)
		: GetVectorParameter(skyMaterial, MATERIAL_COLOR_COLOR, cinema::Vector(1.0)) * GetFloatParameter(skyMaterial, MATERIAL_COLOR_BRIGHTNESS, 1.0);
	cinema::BaseShader* const shader = GetShaderParameter(skyMaterial, luminance ? MATERIAL_LUMINANCE_SHADER : MATERIAL_COLOR_SHADER, materials.GetDocument());

	const maxon::String image = materials.GetImageId(shader, true) iferr_return;
	if (image.IsEmpty())
	{
		if (shader)
			writer.Warn(FormatString("Sky \"@\": the @ shader is not translated yet; the sky uses the channel's plain colour.", sky->GetName(), shader->GetTypeName()));
		return WritePlainWorld(writer, color, 1.0);
	}

	return WritePanoramaWorld(writer, image, skyMg, cinema::Vector(1.0), false);
}

maxon::Result<void> ExportDomeWorld(MaterialExporter& materials, const cinema::BaseObject* dome, const cinema::Matrix& domeMg)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const maxon::Float64 strength = maxon::Pow(2.0, GetFloatParameter(dome, REDSHIFT_LIGHT_DOME_EXPOSURE0, 0.0));
	const cinema::Vector color = GetVectorParameter(dome, REDSHIFT_LIGHT_DOME_COLOR, cinema::Vector(1.0));

	// The texture is a parameter of Redshift's own file type (1036765), and the path one of its
	// parts: both levels of the id must name their type, or nothing comes back. The path is a URL.
	static const maxon::Int32 REDSHIFT_FILE_TYPE = 1036765;
	maxon::String path;
	cinema::GeData data;
	if (dome->GetParameter(CreateDescID(cinema::DescLevel(REDSHIFT_LIGHT_DOME_TEX0, REDSHIFT_FILE_TYPE, 0), cinema::DescLevel(REDSHIFT_FILE_PATH, cinema::DTYPE_STRING, 0)),
		data, cinema::DESCFLAGS_GET::NONE) && data.GetType() == cinema::DA_STRING)
	{
		path = data.GetString();
		if (path.StartsWith("file://"_s))
		{
			const maxon::Url url(path);
			path = url.GetSystemPath() iferr_return;
		}
	}
	if (path.IsEmpty())
		return WritePlainWorld(writer, color, strength);

	cinema::Filename resolved;
	if (!cinema::GenerateTexturePath(materials.GetDocument()->GetDocumentPath(), cinema::Filename(path), cinema::Filename(), &resolved))
		resolved = cinema::Filename(path);
	const maxon::String image = materials.GetFileImageId(resolved.GetString(), true) iferr_return;
	if (GetIntParameter(dome, REDSHIFT_LIGHT_DOME_ENVTYPE, 0) != 0)
		writer.Warn(FormatString("Light \"@\": only a spherical dome map is translated; the map is used as one.", dome->GetName()));
	// With a texture the colour is not used by Redshift; the exposure scales the image.
	// A dome map runs the other way round than a Sky object's panorama (measured against Redshift:
	// tests/c4d/redshift_dome.py); "Flip Horizontal" turns it back.
	return WritePanoramaWorld(writer, image, domeMg, cinema::Vector(strength), !GetBoolParameter(dome, REDSHIFT_LIGHT_DOME_FLIP_HORIZONTAL, false));
}

} // namespace cyclesrender
