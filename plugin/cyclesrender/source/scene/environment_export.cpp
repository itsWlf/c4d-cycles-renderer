// SPDX-License-Identifier: Apache-2.0
// The `world` item: a Sky object's panorama, a Redshift dome light, the Cycles Environment object, or
// the plain default world of the Render Settings; what the camera sees and what lights the scene may
// come from two of them, joined by a Light Path node (environment_export.h).
//
// Reading order: the parts each add their nodes to one graph and hand back their Background's output
// (AddBackground, AddPanorama, AddSkyObject, AddDome, AddEnvironment); ExportWorldItem joins them.

#include "c4d.h"
#include "maxon/url.h"
#include "drsfile.h"
#include "mmaterial.h"
#include "orslight.h"
#include "ttexture.h"
#include "xbitmap.h"

#include "ocyclesenvironment.h"

#include "../ids.h"
#include "../sdk_compat.h"
#include "../materials/cycles_node_material.h"
#include "environment_export.h"
#include "light_export.h"
#include "parameters.h"
#include "redshift_ids.h"
#include "../session/blender_install.h"
#include "../session/json_writer.h"

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

static void NoteImage(maxon::String& images, const maxon::String& image)
{
	if (image.IsEmpty())
		return;
	images += images.IsEmpty() ? image : ";"_s + image;
}

// A plain colour.
static maxon::Result<NodeOutput> AddBackground(NodeGraph& graph, const cinema::Vector& color, maxon::Float64 strength)
{
	iferr_scope;
	const maxon::Int background = graph.Add("ShaderNodeBackground") iferr_return;
	graph.SetInput(background, "Color", color);
	graph.SetInput(background, "Strength", strength);
	return NodeOutput { background, "Background"_s };
}

// A panorama image, turned with the object that carries it.
// @param[in] tint           Multiplies the image (a strength and a colour).
// @param[in] asBlender      False for a Sky object's panorama (mirrored in X against Blender's); true for a map that runs as
//                           Blender's does (a Redshift dome without Flip Horizontal).
// @param[in] projection     The Environment Texture's projection (Blender's identifier).
// @param[in] interpolation  Its interpolation, or empty for Blender's default.
static maxon::Result<NodeOutput> AddPanorama(NodeGraph& graph, const maxon::String& image, const cinema::Matrix& skyMg, const cinema::Vector& tint, maxon::Bool asBlender,
	const maxon::String& projection = "EQUIRECTANGULAR"_s, const maxon::String& interpolation = maxon::String())
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
	const maxon::Float64 k = asBlender ? 1.0 : -1.0;
	const cinema::Vector column0(rx.x, k * ry.x, k * rz.x);
	const cinema::Vector column1(k * rx.y, ry.y, rz.y);
	const cinema::Vector column2(k * rx.z, ry.z, rz.z);
	const cinema::Vector rotation = EulerXYZ(column0, column1, column2);

	const maxon::Int coordinates = graph.Add("ShaderNodeTexCoord") iferr_return;
	const maxon::Int mapping = graph.Add("ShaderNodeMapping") iferr_return;
	const maxon::Int environment = graph.Add("ShaderNodeTexEnvironment") iferr_return;
	const maxon::Int background = graph.Add("ShaderNodeBackground") iferr_return;
	graph.SetProperty(mapping, "vector_type", "POINT"_s);
	graph.SetInput(mapping, "Scale", cinema::Vector(asBlender ? 1.0 : -1.0, 1.0, 1.0));
	graph.SetInput(mapping, "Rotation", rotation);
	graph.SetProperty(environment, "image", image);
	graph.SetProperty(environment, "projection", projection);
	if (interpolation.IsPopulated())
		graph.SetProperty(environment, "interpolation", interpolation);
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
	return NodeOutput { background, "Background"_s };
}

// A Sky object: its material's luminance channel, or its colour channel when luminance is off.
static maxon::Result<NodeOutput> AddSkyObject(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* sky, const cinema::Matrix& skyMg,
	cinema::BaseMaterial* skyMaterial, maxon::String& images)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const DocumentColors& colors = materials.GetColors();
	if (!skyMaterial || skyMaterial->GetType() != Mmaterial)
	{
		// Cinema 4D renders a Sky without a material as plain white.
		if (skyMaterial)
			writer.Warn(FormatString("Sky \"@\": only classic materials are translated for the sky so far; a white sky is used.", sky->GetName()));
		return AddBackground(graph, cinema::Vector(1.0), 1.0);
	}
	const maxon::Bool luminance = GetBoolParameter(skyMaterial, MATERIAL_USE_LUMINANCE, false);
	const cinema::Vector color = colors.Input(luminance
		? GetVectorParameter(skyMaterial, MATERIAL_LUMINANCE_COLOR, cinema::Vector(1.0)) * GetFloatParameter(skyMaterial, MATERIAL_LUMINANCE_BRIGHTNESS, 1.0)
		: GetVectorParameter(skyMaterial, MATERIAL_COLOR_COLOR, cinema::Vector(1.0)) * GetFloatParameter(skyMaterial, MATERIAL_COLOR_BRIGHTNESS, 1.0));
	cinema::BaseShader* const shader = GetShaderParameter(skyMaterial, luminance ? MATERIAL_LUMINANCE_SHADER : MATERIAL_COLOR_SHADER, materials.GetDocument());

	maxon::String image = materials.GetImageId(shader, true) iferr_return;
	if (image.IsEmpty() && shader && shader->GetType() == Xbitmap && GetFilenameParameter(shader, BITMAPSHADER_FILENAME).IsPopulated())
	{
		// A file Blender cannot load as the document's colour settings read it (a linear
		// panorama without Linear Workflow): baked, the picture holds what Cinema 4D uses.
		image = materials.GetBakedImageId(shader) iferr_return;
	}
	if (image.IsEmpty())
	{
		if (shader)
			writer.Warn(FormatString("Sky \"@\": the @ shader is not translated yet; the sky uses the channel's plain colour.", sky->GetName(), shader->GetTypeName()));
		return AddBackground(graph, color, 1.0);
	}
	NoteImage(images, image);
	return AddPanorama(graph, image, skyMg, cinema::Vector(1.0), false);
}

// A Redshift dome light: its map as a spherical panorama turned with the light, times its Intensity and
// 2^exposure; without a map, its colour times its Intensity. Measured with Redshift 2027.0.0, seen by the camera
// (internal/f13/probe_batch1.py, "dome"): the Intensity (REDSHIFT_LIGHT_DOME_MULTIPLIER, 12024) multiplies the map and
// the colour alike (2: 2.000, 0.3: 0.300), the exposure only the map (a colour of 0.5 at Intensity 2 and exposure 1
// shows 1.0); before 0.3.0 neither the Intensity was read nor the exposure kept off the colour. In a document with
// Basic colour management Redshift decodes the colour as a colour parameter (0.5 shows 0.214, 2 shows 4.95: the curve
// continued above 1), not the map.
static maxon::Result<NodeOutput> AddDome(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* dome, const cinema::Matrix& domeMg, maxon::String& images)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const maxon::Float64 intensity = maxon::Max(0.0, GetFloatParameter(dome, REDSHIFT_LIGHT_DOME_MULTIPLIER, 1.0));
	const maxon::Float64 strength = intensity * maxon::Pow(2.0, GetFloatParameter(dome, REDSHIFT_LIGHT_DOME_EXPOSURE0, 0.0));
	const cinema::Vector color = materials.GetColors().Input(GetVectorParameter(dome, REDSHIFT_LIGHT_DOME_COLOR, cinema::Vector(1.0)));

	// The texture is a parameter of Redshift's own file type (REDSHIFT_FILE_TYPE), and the path one of
	// its parts: both levels of the id must name their type, or nothing comes back. The path is a URL.
	maxon::String path;
	cinema::GeData data;
	if (dome->GetParameter(CreateDescID(cinema::DescLevel(REDSHIFT_LIGHT_DOME_TEX0, REDSHIFT_FILE_TYPE, 0), cinema::DescLevel(REDSHIFT_FILE_PATH, cinema::DTYPE_STRING, 0)),
		data, cinema::DESCFLAGS_GET::NONE) && data.GetType() == cinema::DA_STRING)
	{
		path = data.GetString();
	}
	if (path.IsEmpty())
		return AddBackground(graph, color, intensity);

	// As a path Blender can open: a "file://" URL as its path, a map of the Asset Browser ("asset:///...")
	// copied out of its database, a name looked for as Cinema 4D looks for a bitmap's.
	maxon::String problem;
	const maxon::String resolved = materials.ResolveImagePath(path, problem) iferr_return;
	if (problem.IsPopulated())
		writer.Warn(FormatString("Light \"@\": its dome map: @", dome->GetName(), problem));
	if (resolved.IsEmpty())
		return AddBackground(graph, color, intensity);
	const maxon::String image = materials.GetFileImageId(resolved, true) iferr_return;
	NoteImage(images, image);
	if (GetIntParameter(dome, REDSHIFT_LIGHT_DOME_ENVTYPE, 0) != 0)
		writer.Warn(FormatString("Light \"@\": only a spherical dome map is translated; the map is used as one.", dome->GetName()));
	// With a texture the colour is not used by Redshift; the exposure scales the image.
	// A dome map runs the other way round than a Sky object's panorama (measured against Redshift:
	// tests/c4d/redshift_dome.py); "Flip Horizontal" turns it back.
	return AddPanorama(graph, image, domeMg, cinema::Vector(strength), !GetBoolParameter(dome, REDSHIFT_LIGHT_DOME_FLIP_HORIZONTAL, false));
}

// Cinema 4D's Background object (0.3.0): what the camera sees where nothing else is, unlit. Measured with the Standard
// renderer in an OCIO document (internal/f13/probe_batch1.py, "background"): its material's colour channel (the colour
// times its brightness: 2 gives twice) and its luminance channel added ((0.8, 0.2, 0.1) and (0.1, 0.7, 0.2) show
// (0.9, 0.9, 0.3)), the same under a light; without a material the grey of an object without one (0.604 in 2027). A
// shader in either channel is not translated (named); its plain colour counts.
static maxon::Result<NodeOutput> AddBackgroundObject(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* background, cinema::BaseMaterial* material)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const DocumentColors& colors = materials.GetColors();
	if (!material || material->GetType() != Mmaterial)
	{
		if (material)
		{
			writer.Warn(FormatString("Background \"@\": only classic materials are translated for the Background object; it shows the grey of an object without a material.",
				background->GetName()));
		}
		return AddBackground(graph, cinema::Vector(colors.IsOcio() ? DEFAULT_GREY_OCIO : 0.604), 1.0);
	}
	struct Channel
	{
		maxon::Int32 use;
		maxon::Int32 color;
		maxon::Int32 brightness;
		maxon::Int32 shader;
		maxon::Bool on;              // the switch's default
		const maxon::Char* name;
	};
	static const Channel CHANNELS[] = {
		{ MATERIAL_USE_COLOR, MATERIAL_COLOR_COLOR, MATERIAL_COLOR_BRIGHTNESS, MATERIAL_COLOR_SHADER, true, "Color" },
		{ MATERIAL_USE_LUMINANCE, MATERIAL_LUMINANCE_COLOR, MATERIAL_LUMINANCE_BRIGHTNESS, MATERIAL_LUMINANCE_SHADER, false, "Luminance" } };
	cinema::Vector color(0.0);
	for (const Channel& channel : CHANNELS)
	{
		if (!GetBoolParameter(material, channel.use, channel.on))
			continue;
		color += colors.Input(GetVectorParameter(material, channel.color, cinema::Vector(1.0)) * GetFloatParameter(material, channel.brightness, 1.0));
		if (cinema::BaseShader* const shader = GetShaderParameter(material, channel.shader, materials.GetDocument()))
		{
			writer.Warn(FormatString("Background \"@\": the @ shader in the @ channel of its material is not translated; the channel's plain colour is shown.",
				background->GetName(), shader->GetTypeName(), maxon::String(channel.name)));
		}
	}
	return AddBackground(graph, color, 1.0);
}

// --- the Cycles Environment object -------------------------------------------------------------------

// Its settings that are settings of Blender's nodes, in the nodes' order. Only the parameter ids are
// written here (they are stored in scenes); what each setting is comes from the node table.
static const EnvironmentNodeSetting ENVIRONMENT_NODE_SETTINGS[] =
{
	{ CYCLESENV_SKY_TYPE, "ShaderNodeTexSky", "sky_type" },
	{ CYCLESENV_SKY_SUN_DISC, "ShaderNodeTexSky", "sun_disc" },
	{ CYCLESENV_SKY_SUN_SIZE, "ShaderNodeTexSky", "sun_size" },
	{ CYCLESENV_SKY_SUN_INTENSITY, "ShaderNodeTexSky", "sun_intensity" },
	{ CYCLESENV_SKY_SUN_ELEVATION, "ShaderNodeTexSky", "sun_elevation" },
	{ CYCLESENV_SKY_SUN_ROTATION, "ShaderNodeTexSky", "sun_rotation" },
	{ CYCLESENV_SKY_ALTITUDE, "ShaderNodeTexSky", "altitude" },
	{ CYCLESENV_SKY_AIR_DENSITY, "ShaderNodeTexSky", "air_density" },
	{ CYCLESENV_SKY_AEROSOL_DENSITY, "ShaderNodeTexSky", "aerosol_density" },
	{ CYCLESENV_SKY_OZONE_DENSITY, "ShaderNodeTexSky", "ozone_density" },
	{ CYCLESENV_SKY_TURBIDITY, "ShaderNodeTexSky", "turbidity" },
	{ CYCLESENV_SKY_GROUND_ALBEDO, "ShaderNodeTexSky", "ground_albedo" },
	{ CYCLESENV_HDRI_COLORSPACE, "ShaderNodeTexEnvironment", "image.colorspace" },
	{ CYCLESENV_HDRI_PROJECTION, "ShaderNodeTexEnvironment", "projection" },
	{ CYCLESENV_HDRI_INTERPOLATION, "ShaderNodeTexEnvironment", "interpolation" },
};

maxon::Block<const EnvironmentNodeSetting> GetEnvironmentNodeSettings()
{
	return maxon::Block<const EnvironmentNodeSetting>(ENVIRONMENT_NODE_SETTINGS, SIZEOF(ENVIRONMENT_NODE_SETTINGS) / SIZEOF(ENVIRONMENT_NODE_SETTINGS[0]));
}

const CyclesProperty* FindEnvironmentProperty(const EnvironmentNodeSetting& setting)
{
	for (const CyclesNode& node : GetCyclesNodes())
	{
		if (maxon::CString(node.type) != maxon::CString(setting.node))
			continue;
		for (maxon::Int i = 0; i < node.propertyCount; ++i)
		{
			if (maxon::CString(node.properties[i].identifier) == maxon::CString(setting.identifier))
				return &node.properties[i];
		}
	}
	return nullptr;
}

maxon::Result<maxon::BaseArray<EnvironmentOption>> GetEnvironmentOptions(const CyclesProperty& property)
{
	iferr_scope;
	maxon::BaseArray<EnvironmentOption> options;
	maxon::BaseArray<maxon::String> items;
	maxon::String(property.items).Split(";"_s, true, items) iferr_return;
	for (const maxon::String& item : items)
	{
		maxon::BaseArray<maxon::String> parts;
		item.Split("|"_s, false, parts) iferr_return;
		if (parts.IsEmpty())
			continue;
		EnvironmentOption option;
		option.identifier = parts[0];
		option.label = parts.GetCount() > 1 ? parts[1] : parts[0];
		if (parts.GetCount() > 2)
		{
			iferr (option.since = parts[2].ToInt32())
				option.since = 0;
		}
		options.Append(std::move(option)) iferr_return;
	}
	return options;
}

const cinema::BaseMaterial* GetEnvironmentWorldMaterial(const cinema::BaseObject* environment, maxon::Int* count)
{
	// As texture tags stack, a later tag covers an earlier one: the last world material counts.
	const cinema::BaseMaterial* found = nullptr;
	maxon::Int worlds = 0;
	for (const cinema::BaseTag* tag = environment ? environment->GetFirstTag() : nullptr; tag; tag = tag->GetNext())
	{
		if (!tag->IsInstanceOf(Ttexture))
			continue;
		const cinema::BaseMaterial* const material = static_cast<const cinema::TextureTag*>(tag)->GetMaterial();
		if (IsCyclesWorldMaterial(material))
		{
			found = material;
			++worlds;
		}
	}
	if (count)
		*count = worlds;
	return found;
}

// What the texture tags of an environment hold that it does not use: materials that are no world
// material, a world material on a type that is not Material, more than one world material. One line each.
static void NameEnvironmentTags(SceneWriter& writer, const cinema::BaseObject* environment, maxon::Bool materialType)
{
	maxon::Int worlds = 0;
	GetEnvironmentWorldMaterial(environment, &worlds);
	for (const cinema::BaseTag* tag = environment->GetFirstTag(); tag; tag = tag->GetNext())
	{
		if (!tag->IsInstanceOf(Ttexture))
			continue;
		const cinema::BaseMaterial* const material = static_cast<const cinema::TextureTag*>(tag)->GetMaterial();
		if (material && !IsCyclesWorldMaterial(material))
		{
			writer.Warn(FormatString("Environment \"@\": the material \"@\" on it is no Cycles world material (a material of Cycles nodes with a World Output) and is not used.",
				environment->GetName(), material->GetName()));
		}
	}
	if (!materialType && worlds > 0)
	{
		writer.Warn(FormatString("Environment \"@\" has a world material but its type is not Material; set the type to Material to use it.", environment->GetName()));
	}
	else if (materialType && worlds > 1)
	{
		writer.Warn(FormatString("Environment \"@\" has @ world materials; the last texture tag's is used, as a later tag covers an earlier one.", environment->GetName(), worlds));
	}
}

// The Material type: the world of the Cycles world material on the object's texture tag (dragged there
// as any material). Its own nodes decide everything: the object's rotation and strength do not apply.
static maxon::Result<NodeOutput> AddEnvironmentMaterial(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* environment,
	const cinema::Vector& defaultColor, maxon::Float64 defaultStrength)
{
	iferr_scope;
	SceneWriter& writer = materials.GetWriter();
	cinema::BaseMaterial* const material = const_cast<cinema::BaseMaterial*>(GetEnvironmentWorldMaterial(environment, nullptr));
	if (!material)
	{
		writer.Warn(FormatString("Environment \"@\": its type is Material but it has no Cycles world material; the Render Settings' world is used.", environment->GetName()));
		return AddBackground(graph, materials.GetColors().Input(defaultColor), defaultStrength);
	}
	NodeOutput surface;
	NodeOutput displacement;
	CyclesMaterialSettings settings;
	CyclesWorldOutputs world;
	BuildCyclesNodeSurface(materials, graph, material, surface, displacement, settings, &world) iferr_return;
	if (!world.found)
	{
		// No World Output is for the engine that renders (their Target): said by BuildCyclesNodeSurface.
		return AddBackground(graph, cinema::Vector(0.0), 0.0);
	}
	if (!world.surface.IsValid())
	{
		writer.Warn(FormatString("Environment \"@\": nothing is wired into the Surface of the World Output of \"@\"; the world is black.", environment->GetName(), material->GetName()));
		return AddBackground(graph, cinema::Vector(0.0), 0.0);
	}
	if (world.volume.IsValid())
		writer.Warn(FormatString("Environment \"@\": the Volume of the World Output of \"@\" is not used; the World Fog tab gives the world's fog.", environment->GetName(), material->GetName()));
	return world.surface;
}


maxon::Bool IsEnvironmentSwitchedOn(const cinema::BaseObject* environment)
{
	return environment && environment->GetDeformMode();
}

EnvironmentRoles GetEnvironmentRoles(const cinema::BaseObject* environment)
{
	EnvironmentRoles roles;
	if (!IsEnvironmentSwitchedOn(environment))
		return roles;
	roles.camera = GetBoolParameter(environment, CYCLESENV_SEEN_BY_CAMERA, true);
	roles.lighting = GetBoolParameter(environment, CYCLESENV_LIGHTS_SCENE, true);
	roles.fog = GetBoolParameter(environment, CYCLESENV_FOG_ENABLE, false);
	return roles;
}

const cinema::BaseObject* GetEnvironmentSunLight(const cinema::BaseObject* environment, const cinema::BaseDocument* doc, maxon::Bool& notALight)
{
	notALight = false;
	if (!environment || !doc)
		return nullptr;
	const cinema::GeData data = GetParameterData(environment, CYCLESENV_SKY_SUN_LIGHT);
	const cinema::BaseList2D* const linked = data.GetType() == cinema::DA_ALIASLINK ? data.GetLink(doc) : nullptr;
	if (!linked)
		return nullptr;
	const cinema::Int32 type = linked->GetType();
	if (type != Olight && type != Orslight && type != ID_LIGHT_OBJECT)
	{
		notALight = true;
		return nullptr;
	}
	return static_cast<const cinema::BaseObject*>(linked);
}

// The sun of the settings (elevation and rotation as Blender's, in the object's axes) turned by the
// object: a unit direction in Cinema 4D's axes. Blender puts the sun at (cos e sin r, cos e cos r, sin e)
// in its own axes (rotation 0 towards +Y, 90° towards +X; Cycles' svm_sky.h with the node's
// simplify_settings), which is (cos e sin r, sin e, cos e cos r) in Cinema 4D's. With a light linked, the
// sun lies where the light shines from: against its +Z axis, along which Cinema 4D's lights, Redshift's and
// the Cycles Light shine (scene/coords.h, AimedMatrix), in the world.
cinema::Vector GetEnvironmentSunDirection(const cinema::BaseObject* environment, const cinema::Matrix& mg, const cinema::BaseDocument* doc)
{
	maxon::Bool notALight = false;
	if (const cinema::BaseObject* const light = GetEnvironmentSunLight(environment, doc, notALight))
	{
		const cinema::Vector shines = light->GetMg().sqmat.v3;
		if (shines.GetLength() > 0.0)
			return -shines.GetNormalized();
	}
	const maxon::Float64 elevation = GetFloatParameter(environment, CYCLESENV_SKY_SUN_ELEVATION, 0.0);
	const maxon::Float64 rotation = GetFloatParameter(environment, CYCLESENV_SKY_SUN_ROTATION, 0.0);
	const cinema::Vector local(maxon::Cos(elevation) * maxon::Sin(rotation), maxon::Sin(elevation), maxon::Cos(elevation) * maxon::Cos(rotation));
	const cinema::Vector world = mg.sqmat.v1.GetNormalized() * local.x + mg.sqmat.v2.GetNormalized() * local.y + mg.sqmat.v3.GetNormalized() * local.z;
	return world.GetLength() > 0.0 ? world.GetNormalized() : cinema::Vector(0.0, 1.0, 0.0);
}

static maxon::String NumberJson(maxon::Float64 value)
{
	return maxon::String::FloatToString(value, -1, 9);
}

static maxon::Result<NodeOutput> AddSkyTexture(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* environment, const cinema::Matrix& mg, maxon::Float64 strength)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const maxon::Int32 blender = materials.GetBlenderVersion();
	const maxon::Int sky = graph.Add("ShaderNodeTexSky") iferr_return;
	const maxon::Int background = graph.Add("ShaderNodeBackground") iferr_return;

	maxon::Bool notALight = false;
	GetEnvironmentSunLight(environment, materials.GetDocument(), notALight);
	if (notALight)
		writer.Warn(FormatString("Environment \"@\": the object linked as the sun is no light; the Sky Texture's own sun is used.", environment->GetName()));
	// The sun turned by the object (or the linked light's), as Blender's elevation and rotation (and direction, for the older skies).
	const cinema::Vector sun = GetEnvironmentSunDirection(environment, mg, materials.GetDocument());
	const maxon::Float64 elevation = maxon::ASin(maxon::ClampValue(sun.y, -1.0, 1.0));
	maxon::Float64 rotation = maxon::ATan2(sun.x, sun.z);
	if (rotation < 0.0)
		rotation += 2.0 * maxon::PI;

	for (const EnvironmentNodeSetting& setting : GetEnvironmentNodeSettings())
	{
		if (maxon::CString(setting.node) != "ShaderNodeTexSky"_cs)
			continue;
		const CyclesProperty* const property = FindEnvironmentProperty(setting);
		if (!property)
			continue;
		if (blender != 0 && property->since > blender)
		{
			writer.Warn(FormatString("Environment \"@\": the Sky Texture's @ needs Blender @ or newer and is left out.", environment->GetName(),
				maxon::String(property->label), BlenderVersionName(property->since)));
			continue;
		}
		switch (property->kind)
		{
			case 'e':
			{
				const maxon::BaseArray<EnvironmentOption> options = GetEnvironmentOptions(*property) iferr_return;
				const maxon::Int32 index = GetIntParameter(environment, setting.id, 0);
				if (index < 0 || index >= options.GetCount())
					break;
				const EnvironmentOption& option = options[index];
				if (blender != 0 && option.since > blender)
				{
					writer.Warn(FormatString("Environment \"@\": the Sky Texture's @ \"@\" needs Blender @ or newer; Blender's default is used.", environment->GetName(),
						maxon::String(property->label), option.label, BlenderVersionName(option.since)));
					break;
				}
				graph.SetProperty(sky, property->identifier, option.identifier);
				break;
			}
			case 'b':
				graph.SetProperty(sky, property->identifier, GetBoolParameter(environment, setting.id, property->number != 0.0));
				break;
			default:
			{
				maxon::Float64 value = GetFloatParameter(environment, setting.id, property->number);
				if (setting.id == CYCLESENV_SKY_SUN_ELEVATION)
					value = elevation;
				else if (setting.id == CYCLESENV_SKY_SUN_ROTATION)
					value = rotation;
				graph.SetPropertyJson(sky, property->identifier, NumberJson(value));
				break;
			}
		}
	}
	// Preetham and Hosek / Wilkie take the sun as a direction (Blender's axes), which the node table does
	// not list (it is no setting of the node's panel but a widget): the same sun.
	graph.SetPropertyJson(sky, "sun_direction", FormatString("[@, @, @]", NumberJson(sun.x), NumberJson(sun.z), NumberJson(sun.y)));
	graph.SetInput(background, "Strength", strength);
	graph.Link(NodeOutput { sky, "Color"_s }, background, "Color") iferr_return;
	return NodeOutput { background, "Background"_s };
}

// An option of the HDRI's settings as Blender's identifier, or empty.
static maxon::Result<maxon::String> HdriOption(const cinema::BaseObject* environment, maxon::Int32 id)
{
	iferr_scope;
	for (const EnvironmentNodeSetting& setting : GetEnvironmentNodeSettings())
	{
		if (setting.id != id)
			continue;
		const CyclesProperty* const property = FindEnvironmentProperty(setting);
		if (!property)
			return maxon::String();
		const maxon::BaseArray<EnvironmentOption> options = GetEnvironmentOptions(*property) iferr_return;
		const maxon::Int32 index = GetIntParameter(environment, id, 0);
		return index >= 0 && index < options.GetCount() ? options[index].identifier : maxon::String();
	}
	return maxon::String();
}

// A Cycles Environment: its type's world.
static maxon::Result<NodeOutput> AddEnvironment(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* environment, const cinema::Matrix& mg,
	const cinema::Vector& defaultColor, maxon::Float64 defaultStrength, maxon::String& images)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	const maxon::Float64 strength = GetFloatParameter(environment, CYCLESENV_STRENGTH, 1.0);
	const maxon::Int32 type = GetIntParameter(environment, CYCLESENV_TYPE, CYCLESENV_TYPE_SKY);
	NameEnvironmentTags(writer, environment, type == CYCLESENV_TYPE_MATERIAL);
	switch (type)
	{
		case CYCLESENV_TYPE_SKY:
			return AddSkyTexture(materials, graph, environment, mg, strength);
		case CYCLESENV_TYPE_HDRI:
		{
			const cinema::Filename file = GetFilenameParameter(environment, CYCLESENV_HDRI_FILE);
			if (!file.IsPopulated())
			{
				writer.Warn(FormatString("Environment \"@\": no HDRI file is chosen; the world is black.", environment->GetName()));
				return AddBackground(graph, cinema::Vector(0.0), 0.0);
			}
			// Found as a bitmap's file is: next to the document, in its "tex" folder, in the search paths.
			cinema::Filename resolved;
			maxon::Int urlMark = 0;
			if (file.GetString().Find("://"_s, &urlMark))
			{
				// A file of the Asset Browser: copied out, so that Blender can open it.
				maxon::String problem;
				const maxon::String copied = materials.ResolveImagePath(file.GetString(), problem) iferr_return;
				if (problem.IsPopulated())
					writer.Warn(FormatString("Environment \"@\": its HDRI file: @", environment->GetName(), problem));
				resolved = cinema::Filename(copied.IsPopulated() ? copied : file.GetString());
			}
			else if (!cinema::GenerateTexturePath(materials.GetDocument()->GetDocumentPath(), file, cinema::Filename(), &resolved))
			{
				resolved = file;
				writer.Warn(FormatString("Environment \"@\": the HDRI file \"@\" was not found.", environment->GetName(), file.GetString()));
			}
			const maxon::String colorSpace = HdriOption(environment, CYCLESENV_HDRI_COLORSPACE) iferr_return;
			const maxon::String image = materials.GetFileImageId(resolved.GetString(), colorSpace != "DATA"_s) iferr_return;
			NoteImage(images, image);
			maxon::String projection = HdriOption(environment, CYCLESENV_HDRI_PROJECTION) iferr_return;
			if (projection.IsEmpty())
				projection = "EQUIRECTANGULAR"_s;
			const maxon::String interpolation = HdriOption(environment, CYCLESENV_HDRI_INTERPOLATION) iferr_return;
			// Turned as a Sky object's panorama is, so that one file shows the same on either.
			return AddPanorama(graph, image, mg, cinema::Vector(strength), false, projection, interpolation);
		}
		case CYCLESENV_TYPE_MATERIAL:
			return AddEnvironmentMaterial(materials, graph, environment, defaultColor, defaultStrength);
		default:
		{
			const cinema::Vector color = materials.GetColors().Input(GetVectorParameter(environment, CYCLESENV_COLOR, cinema::Vector(0.05)));
			return AddBackground(graph, color, strength);
		}
	}
}

// One part's world, or black for a role nothing fills.
static maxon::Result<NodeOutput> AddPart(MaterialExporter& materials, NodeGraph& graph, const WorldPart& part, const cinema::Vector& defaultColor,
	maxon::Float64 defaultStrength, maxon::String& images)
{
	switch (part.kind)
	{
		case WORLDPART::SKY: return AddSkyObject(materials, graph, part.object, part.mg, part.material, images);
		case WORLDPART::DOME: return AddDome(materials, graph, part.object, part.mg, images);
		case WORLDPART::ENVIRONMENT: return AddEnvironment(materials, graph, part.object, part.mg, defaultColor, defaultStrength, images);
		case WORLDPART::BACKGROUND: return AddBackgroundObject(materials, graph, part.object, part.material);
		default: return AddBackground(graph, cinema::Vector(0.0), 0.0);
	}
}

// The World Fog of a Cycles Environment: a Principled Volume with the fog object's values (scene/fog_export.cpp).
static maxon::Result<NodeOutput> AddWorldFog(MaterialExporter& materials, NodeGraph& graph, const cinema::BaseObject* environment)
{
	iferr_scope;
	const DocumentColors& colors = materials.GetColors();
	const maxon::Int shader = graph.Add("ShaderNodeVolumePrincipled") iferr_return;
	graph.SetInputJson(shader, "Density Attribute", JsonQuoted(maxon::String()));
	graph.SetInputJson(shader, "Color Attribute", JsonQuoted(maxon::String()));
	graph.SetInputJson(shader, "Temperature Attribute", JsonQuoted(maxon::String()));
	graph.SetInput(shader, "Density", GetFloatParameter(environment, CYCLESENV_FOG_DENSITY, ENVIRONMENT_DEFAULT_FOG_DENSITY));
	graph.SetInput(shader, "Color", colors.Input(GetVectorParameter(environment, CYCLESENV_FOG_COLOR, cinema::Vector(0.5))));
	graph.SetInput(shader, "Anisotropy", GetFloatParameter(environment, CYCLESENV_FOG_ANISOTROPY, 0.0));
	graph.SetInput(shader, "Absorption Color", colors.Input(GetVectorParameter(environment, CYCLESENV_FOG_ABSORPTION_COLOR, cinema::Vector(0.0))));
	graph.SetInput(shader, "Emission Strength", GetFloatParameter(environment, CYCLESENV_FOG_EMISSION_STRENGTH, 0.0));
	graph.SetInput(shader, "Emission Color", colors.Input(GetVectorParameter(environment, CYCLESENV_FOG_EMISSION_COLOR, cinema::Vector(1.0))));
	graph.SetInput(shader, "Blackbody Intensity", 0.0);
	return NodeOutput { shader, "Volume"_s };
}

// Whether a Redshift light switched on (its green check) is among @p op, the objects after it and below them.
static maxon::Bool HasRedshiftLight(const cinema::BaseObject* op)
{
	for (; op; op = op->GetNext())
	{
		if (op->GetType() == Orslight && IsLightSwitchedOn(op))
			return true;
		if (HasRedshiftLight(op->GetDown()))
			return true;
	}
	return false;
}

maxon::Result<void> ExportWorldItem(MaterialExporter& materials, const WorldPart& camera, const WorldPart& lighting, const WorldPart& fog,
	const cinema::Vector& defaultColor, maxon::Float64 defaultStrength, maxon::String* images)
{
	iferr_scope;

	SceneWriter& writer = materials.GetWriter();
	maxon::String read;
	NodeGraph graph;
	const maxon::Int output = graph.Add("ShaderNodeOutputWorld") iferr_return;
	NodeOutput surface;
	if (camera.kind == WORLDPART::NONE && lighting.kind == WORLDPART::NONE)
	{
		// No environment for either role: the Render Settings' world (a colour parameter, read as any). A scene set up
		// for Redshift is black there, as Redshift renders it (0.3.0, F13 W24, for the maintainer to confirm: measured on
		// RS_Parity_PyroSmoke, Redshift's background 0 where the default world gave 0.0039 and lit the scene), while the
		// Render Settings' world is untouched; one set there counts. Set up for Redshift: Redshift's renderer in the
		// render data and a Redshift light switched on (the renderer alone does not tell: every new document of Cinema
		// 4D 2027 holds it, measured).
		const maxon::Bool untouched = defaultColor == RENDER_SETTINGS_WORLD_COLOR && defaultStrength == 1.0;
		const cinema::BaseDocument* const doc = materials.GetDocument();
		if (untouched && FindRedshiftRenderer(doc) && doc && HasRedshiftLight(doc->GetFirstObject()))
		{
			writer.Warn("The scene is set up for Redshift and has no environment: its world is black, as Redshift renders it (a world colour set in the Render Settings is used instead)."_s);
			surface = AddBackground(graph, cinema::Vector(0.0), 0.0) iferr_return;
		}
		else
		{
			surface = AddBackground(graph, materials.GetColors().Input(defaultColor), defaultStrength) iferr_return;
		}
	}
	else if (camera.object == lighting.object)
	{
		surface = AddPart(materials, graph, camera, defaultColor, defaultStrength, read) iferr_return;
	}
	else
	{
		// Two environments: the camera's for camera rays, the other one for every other ray.
		const NodeOutput seen = AddPart(materials, graph, camera, defaultColor, defaultStrength, read) iferr_return;
		const NodeOutput lights = AddPart(materials, graph, lighting, defaultColor, defaultStrength, read) iferr_return;
		const maxon::Int path = graph.Add("ShaderNodeLightPath") iferr_return;
		const maxon::Int mix = graph.Add("ShaderNodeMixShader") iferr_return;
		graph.Link(NodeOutput { path, "Is Camera Ray"_s }, mix, "Fac") iferr_return;
		graph.Link(lights, mix, maxon::Int(1)) iferr_return;
		graph.Link(seen, mix, maxon::Int(2)) iferr_return;
		surface = NodeOutput { mix, "Shader"_s };
	}
	graph.Link(surface, output, "Surface") iferr_return;
	if (fog.kind == WORLDPART::ENVIRONMENT && fog.object)
	{
		const NodeOutput volume = AddWorldFog(materials, graph, fog.object) iferr_return;
		graph.Link(volume, output, "Volume") iferr_return;
	}

	JsonWriter& json = writer.BeginItem("world", "world"_s);
	graph.Write(json);
	writer.EndItem();
	if (images)
		*images = read;
	return maxon::OK;
}

} // namespace cyclesrender
