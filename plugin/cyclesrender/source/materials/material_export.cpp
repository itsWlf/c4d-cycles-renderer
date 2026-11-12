// SPDX-License-Identifier: Apache-2.0
// Materials as scene items (material_export.h).
//
// In this file, in order: how a texture tag places textures (TextureMapping); the classic
// material as a node graph (ClassicMaterialGraph, one method per channel); image items and
// the cache of baked shaders; and at the end the dispatch: WriteLayers() decides which
// translator a material goes to, GetMaterialId() hands out ids and writes each material once.

#include "c4d.h"
#include "c4d_reflection.h"
#include "mmaterial.h"
#include "maxon/spinlock.h"
#include "ttexture.h"
#include "xbitmap.h"

#include "cycles_node_material.h"
#include "../ids.h"
#include "exchange_material.h"
#include "../nodespace/cycles_nodes.h"
#include "redshift_material.h"
#include "shader_translate.h"

#include "../scene/coords.h"
#include "../scene/parameters.h"
#include "baked_files.h"
#include "material_export.h"
#include "native_material.h"

namespace cyclesrender
{

// Blender's names for the two ways an image file is read.
static const maxon::Char* const COLORSPACE_COLOR = "sRGB";
static const maxon::Char* const COLORSPACE_LINEAR_COLOR = "Linear Rec.709";
static const maxon::Char* const COLORSPACE_DATA = "Non-Color";

// File types that store scene-linear values rather than display-encoded ones.
static maxon::Bool IsLinearFile(const cinema::Filename& file)
{
	return file.CheckSuffix("exr"_s) || file.CheckSuffix("hdr"_s);
}

// File types whose alpha channel Cinema 4D's "Image Alpha" option reads.
static maxon::Bool CanHoldAlpha(const cinema::Filename& file)
{
	return file.CheckSuffix("png"_s) || file.CheckSuffix("tif"_s) || file.CheckSuffix("tiff"_s) || file.CheckSuffix("tga"_s) ||
		file.CheckSuffix("psd"_s) || file.CheckSuffix("exr"_s) || file.CheckSuffix("webp"_s);
}

// Sign of the turn around the Y axis along which U grows in spherical and cylindrical
// projection; see ClassicMaterialGraph::Coordinates().
static const maxon::Float64 SPHERICAL_U_DIRECTION = -1.0;
// U at the tag's +X axis.
static const maxon::Float64 SPHERICAL_U_START = 0.5;

TextureMapping TextureMapping::Read(const cinema::BaseTag* tag)
{
	TextureMapping mapping;
	if (!tag || !tag->IsInstanceOf(Ttexture))
		return mapping;
	mapping.projection = GetIntParameter(tag, TEXTURETAG_PROJECTION, TEXTURETAG_PROJECTION_UVW);
	mapping.offsetX = GetFloatParameter(tag, TEXTURETAG_OFFSETX, 0.0);
	mapping.offsetY = GetFloatParameter(tag, TEXTURETAG_OFFSETY, 0.0);
	mapping.lengthX = GetFloatParameter(tag, TEXTURETAG_LENGTHX, 1.0);
	mapping.lengthY = GetFloatParameter(tag, TEXTURETAG_LENGTHY, 1.0);
	mapping.tile = GetBoolParameter(tag, TEXTURETAG_TILE, true);
	if (mapping.projection != TEXTURETAG_PROJECTION_UVW)
		mapping.ml = static_cast<const cinema::TextureTag*>(tag)->GetMl();
	return mapping;
}

maxon::Bool TextureMapping::IsPlain() const
{
	return projection == TEXTURETAG_PROJECTION_UVW && offsetX == 0.0 && offsetY == 0.0 && lengthX == 1.0 && lengthY == 1.0 && tile;
}

maxon::String TextureMapping::GetKey() const
{
	if (IsPlain())
		return maxon::String();
	maxon::String key = FormatString("@/@/@/@/@/@", projection, offsetX, offsetY, lengthX, lengthY, tile);
	if (projection != TEXTURETAG_PROJECTION_UVW)
		key += FormatString("/@/@/@/@", ml.off, ml.sqmat.v1, ml.sqmat.v2, ml.sqmat.v3);
	return key;
}

static maxon::Float64 MaxComponent(const cinema::Vector& v)
{
	return maxon::Max(v.x, maxon::Max(v.y, v.z));
}

//----------------------------------------------------------------------------------------
// Builds the node graph of one classic material around a Principled BSDF.
//----------------------------------------------------------------------------------------
class ClassicMaterialGraph
{
public:
	/// The nodes are added to @p graph, which may hold other materials of a stack already.
	ClassicMaterialGraph(MaterialExporter& exporter, NodeGraph& graph, cinema::Material* material, const TextureMapping& mapping)
		: _exporter(exporter), _graph(graph), _material(material), _mapping(mapping), _shaders(exporter, graph, material->GetName()) { }

	/// Builds the surface. Nothing is linked to an output node; the caller does that.
	maxon::Result<void> Build();
	NodeOutput GetSurface() const { return NodeOutput { _bsdf, "BSDF"_s }; }
	/// How much of the surface there is, 0 to 1; invalid without an alpha channel.
	const NodeOutput& GetAlpha() const { return _alpha; }
	/// For the output's Displacement socket; invalid without a displacement channel.
	const NodeOutput& GetDisplacement() const { return _displacement; }
	/// Lets the surface's own Alpha input do the cutting out (for a material with nothing below it).
	maxon::Result<void> ApplyAlphaToSurface();

private:
	// An image texture node for a bitmap shader. The result is invalid when the shader is
	// missing or of a kind that is not translated yet.
	maxon::Result<NodeOutput> Texture(cinema::BaseShader* shader, maxon::Bool colorData, const maxon::Char* channel, maxon::Bool wantAlpha = false);

	// Colour of a channel: its colour times brightness, combined with its texture as the
	// channel's mix mode and mix strength say. Sets or links the input @p socket of the BSDF.
	maxon::Result<void> ColorChannel(const maxon::Char* socket, const maxon::Char* channel, maxon::Int32 colorId, maxon::Int32 brightnessId,
		maxon::Int32 shaderId, maxon::Int32 mixModeId, maxon::Int32 mixStrengthId, const cinema::Vector& tintTowards, maxon::Float64 tintAmount);

	// A normal for the BSDF made from a height map, the way Cinema 4D's bump channel does it.
	maxon::Result<NodeOutput> Bump(const maxon::String& image, maxon::Float64 strength);

	maxon::Result<void> Reflectance();
	maxon::Result<void> Alpha();
	maxon::Result<void> BumpAndNormal();
	maxon::Result<void> Displacement();
	maxon::Result<void> Coordinates();
	maxon::Result<NodeOutput> TextureSpacePoint(const maxon::Float64 scale[3], const maxon::Float64 shift[3]);

	maxon::Bool Uses(maxon::Int32 channelSwitch) const { return GetBoolParameter(_material, channelSwitch, false); }
	void NotTranslated(const maxon::Char* what)
	{
		_exporter.GetWriter().Warn(FormatString("Material \"@\": @ is not translated yet.", _material->GetName(), what));
	}

	MaterialExporter& _exporter;
	NodeGraph& _graph;
	cinema::Material* _material;
	TextureMapping _mapping;
	ShaderTranslator _shaders;
	maxon::Int _bsdf = NOTOK;
	NodeOutput _alpha;
	NodeOutput _displacement;
};

maxon::Result<NodeOutput> ClassicMaterialGraph::Texture(cinema::BaseShader* shader, maxon::Bool colorData, const maxon::Char* channel, maxon::Bool wantAlpha)
{
	iferr_scope;

	if (wantAlpha)
		return _shaders.ImageAlpha(shader);
	return _shaders.Translate(shader, colorData, channel);
}

maxon::Result<void> ClassicMaterialGraph::ColorChannel(const maxon::Char* socket, const maxon::Char* channel, maxon::Int32 colorId, maxon::Int32 brightnessId,
	maxon::Int32 shaderId, maxon::Int32 mixModeId, maxon::Int32 mixStrengthId, const cinema::Vector& tintTowards, maxon::Float64 tintAmount)
{
	iferr_scope;

	cinema::Vector color = GetVectorParameter(_material, colorId, cinema::Vector(1.0)) * GetFloatParameter(_material, brightnessId, 1.0);
	color = color * (1.0 - tintAmount) + tintTowards * tintAmount;

	const NodeOutput texture = Texture(GetShaderParameter(_material, shaderId, _exporter.GetDocument()), true, channel) iferr_return;
	if (!texture.IsValid())
	{
		_graph.SetInput(_bsdf, socket, color);
		return maxon::OK;
	}

	const maxon::Int32 mode = GetIntParameter(_material, mixModeId, MATERIAL_TEXTUREMIXING_NORMAL);
	const maxon::Float64 strength = GetFloatParameter(_material, mixStrengthId, 1.0);
	if (mode == MATERIAL_TEXTUREMIXING_NORMAL && strength >= 0.999)
	{
		// The texture replaces the colour completely.
		_graph.Link(texture, _bsdf, socket) iferr_return;
		return maxon::OK;
	}

	const maxon::Int mix = _graph.Add("ShaderNodeMix") iferr_return;
	_graph.SetProperty(mix, "data_type", "RGBA"_s);
	switch (mode)
	{
		case MATERIAL_TEXTUREMIXING_ADD: _graph.SetProperty(mix, "blend_type", "ADD"_s); break;
		case MATERIAL_TEXTUREMIXING_SUBTRACT: _graph.SetProperty(mix, "blend_type", "SUBTRACT"_s); break;
		case MATERIAL_TEXTUREMIXING_MULTIPLY: _graph.SetProperty(mix, "blend_type", "MULTIPLY"_s); break;
		default: _graph.SetProperty(mix, "blend_type", "MIX"_s); break;
	}
	_graph.SetInput(mix, "Factor_Float", strength);
	_graph.SetInput(mix, "A_Color", color);
	_graph.Link(texture, mix, "B_Color") iferr_return;
	_graph.Link(NodeOutput { mix, "Result_Color"_s }, _bsdf, socket) iferr_return;
	return maxon::OK;
}

// The reflectance channel is a stack of any number of layers; the Principled BSDF has one
// specular lobe and one clear coat. This is an approximation for the usual one or two layers,
// and is not meant to be complete. Diffuse layers (Lambertian, Oren-Nayar) are skipped. Of the
// others, the first two count:
//
// One layer decides the specular lobe:
//   legacy specular (the "Default Specular" of a new material): highlights only.
//     roughness = width, specular level = 0.5 at the default strength of 20 %.
//   reflection without Fresnel: reflects equally at every angle, like a metal.
//     metallic = reflection strength, and at full strength the layer colour becomes the base colour.
//   dielectric Fresnel: specular level = 0.5 * reflection strength, IOR from the layer.
//   conductor Fresnel: metallic = reflection strength, layer colour becomes the base colour.
//
// With two layers, one of which has a dielectric Fresnel, that one becomes the clear coat
// (weight = reflection strength, with its roughness and IOR) and the other the lobe. If both
// or neither are dielectric, the first is the lobe and the second is left out with a warning,
// as are all layers after the second.
maxon::Result<void> ClassicMaterialGraph::Reflectance()
{
	iferr_scope;

	_graph.SetInput(_bsdf, "Roughness", 0.5);
	_graph.SetInput(_bsdf, "Specular IOR Level", 0.0);
	if (!Uses(MATERIAL_USE_REFLECTION))
		return maxon::OK;

	// The data ids of the layers that reflect.
	maxon::BaseArray<maxon::Int32> layers;
	const maxon::Int32 layerCount = _material->GetReflectionLayerCount();
	for (maxon::Int32 i = 0; i < layerCount; ++i)
	{
		cinema::ReflectionLayer* const layer = _material->GetReflectionLayerIndex(i);
		if (!layer)
			continue;
		const maxon::Int32 distribution = GetIntParameter(_material, layer->GetDataID() + REFLECTION_LAYER_MAIN_DISTRIBUTION, REFLECTION_DISTRIBUTION_GGX);
		if (distribution == REFLECTION_DISTRIBUTION_LAMBERTIAN || distribution == REFLECTION_DISTRIBUTION_ORENNAYAR)
			continue;
		layers.Append(layer->GetDataID()) iferr_return;
	}
	if (layers.IsEmpty())
		return maxon::OK;

	auto isSpecularOnly = [this](maxon::Int32 base)
	{
		const maxon::Int32 distribution = GetIntParameter(_material, base + REFLECTION_LAYER_MAIN_DISTRIBUTION, REFLECTION_DISTRIBUTION_GGX);
		return distribution == REFLECTION_DISTRIBUTION_SPECULAR_BLINN || distribution == REFLECTION_DISTRIBUTION_SPECULAR_PHONG;
	};
	auto isDielectric = [this, &isSpecularOnly](maxon::Int32 base)
	{
		return !isSpecularOnly(base) && GetIntParameter(_material, base + REFLECTION_LAYER_FRESNEL_MODE, REFLECTION_FRESNEL_NONE) == REFLECTION_FRESNEL_DIELECTRIC;
	};

	maxon::Int32 lobe = layers[0];
	maxon::Int32 coat = NOTOK;
	if (layers.GetCount() >= 2)
	{
		if (isDielectric(layers[1]) && !isDielectric(layers[0]))
		{
			coat = layers[1];
		}
		else if (isDielectric(layers[0]) && !isDielectric(layers[1]))
		{
			lobe = layers[1];
			coat = layers[0];
		}
		else
		{
			NotTranslated("the second reflectance layer (two layers of the same kind)");
		}
	}
	if (layers.GetCount() > 2)
		NotTranslated("more than two reflectance layers");

	// --- the specular lobe ---
	{
		const maxon::Int32 base = lobe;
		const maxon::Float64 roughness = maxon::Clamp01(GetFloatParameter(_material, base + REFLECTION_LAYER_MAIN_VALUE_ROUGHNESS, 0.5));
		const maxon::Float64 reflection = maxon::Clamp01(GetFloatParameter(_material, base + REFLECTION_LAYER_MAIN_VALUE_REFLECTION, 1.0));
		const maxon::Float64 specular = GetFloatParameter(_material, base + REFLECTION_LAYER_MAIN_VALUE_SPECULAR, 0.2);
		const cinema::Vector layerColor = GetVectorParameter(_material, base + REFLECTION_LAYER_COLOR_COLOR, cinema::Vector(1.0)) *
			GetFloatParameter(_material, base + REFLECTION_LAYER_COLOR_BRIGHTNESS, 1.0);

		_graph.SetInput(_bsdf, "Roughness", roughness);
		const NodeOutput roughnessTexture = Texture(GetShaderParameter(_material, base + REFLECTION_LAYER_MAIN_SHADER_ROUGHNESS, _exporter.GetDocument()),
			false, "reflectance roughness") iferr_return;
		if (roughnessTexture.IsValid())
		{
			const maxon::Int scale = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(scale, "operation", "MULTIPLY"_s);
			_graph.Link(roughnessTexture, scale, "Value") iferr_return;
			_graph.SetInput(scale, "Value_001", roughness);
			_graph.Link(NodeOutput { scale, "Value"_s }, _bsdf, "Roughness") iferr_return;
		}

		if (isSpecularOnly(base))
		{
			_graph.SetInput(_bsdf, "Specular IOR Level", maxon::Clamp01(0.5 * specular / 0.2));
		}
		else if (isDielectric(base))
		{
			_graph.SetInput(_bsdf, "Specular IOR Level", 0.5 * reflection);
			_graph.SetInput(_bsdf, "IOR", maxon::Max(1.0, GetFloatParameter(_material, base + REFLECTION_LAYER_FRESNEL_VALUE_IOR, 1.5)));
		}
		else
		{
			const maxon::Int32 fresnel = GetIntParameter(_material, base + REFLECTION_LAYER_FRESNEL_MODE, REFLECTION_FRESNEL_NONE);
			_graph.SetInput(_bsdf, "Specular IOR Level", 0.5);
			_graph.SetInput(_bsdf, "Metallic", reflection);
			if (reflection >= 0.99 || fresnel == REFLECTION_FRESNEL_CONDUCTOR)
				_graph.SetInput(_bsdf, "Base Color", layerColor);
		}
	}

	// --- the clear coat ---
	if (coat != NOTOK)
	{
		_graph.SetInput(_bsdf, "Coat Weight", maxon::Clamp01(GetFloatParameter(_material, coat + REFLECTION_LAYER_MAIN_VALUE_REFLECTION, 1.0)));
		_graph.SetInput(_bsdf, "Coat Roughness", maxon::Clamp01(GetFloatParameter(_material, coat + REFLECTION_LAYER_MAIN_VALUE_ROUGHNESS, 0.0)));
		_graph.SetInput(_bsdf, "Coat IOR", maxon::Max(1.0, GetFloatParameter(_material, coat + REFLECTION_LAYER_FRESNEL_VALUE_IOR, 1.5)));
	}
	return maxon::OK;
}

maxon::Result<void> ClassicMaterialGraph::Alpha()
{
	iferr_scope;

	if (!Uses(MATERIAL_USE_ALPHA))
		return maxon::OK;

	cinema::BaseShader* const shader = GetShaderParameter(_material, MATERIAL_ALPHA_SHADER, _exporter.GetDocument());
	if (!shader)
		return maxon::OK;

	// "Image Alpha" reads the file's alpha channel when it has one, otherwise its brightness.
	const maxon::Bool fromAlphaChannel = GetBoolParameter(_material, MATERIAL_ALPHA_IMAGEALPHA, true) && shader->GetType() == Xbitmap &&
		CanHoldAlpha(GetFilenameParameter(shader, BITMAPSHADER_FILENAME)) && _exporter.ImageHasAlpha(shader);
	NodeOutput alpha = Texture(shader, false, "alpha", fromAlphaChannel) iferr_return;
	if (!alpha.IsValid())
		return maxon::OK;

	if (GetBoolParameter(_material, MATERIAL_ALPHA_INVERT, false))
	{
		const maxon::Int invert = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(invert, "operation", "SUBTRACT"_s);
		_graph.SetInput(invert, "Value", 1.0);
		_graph.Link(alpha, invert, "Value_001") iferr_return;
		alpha = NodeOutput { invert, "Value"_s };
	}
	_alpha = alpha;
	return maxon::OK;
}

maxon::Result<void> ClassicMaterialGraph::ApplyAlphaToSurface()
{
	iferr_scope;
	if (_alpha.IsValid())
	{
		_graph.Link(_alpha, _bsdf, "Alpha") iferr_return;
	}
	return maxon::OK;
}

// Cinema 4D's bump works in the texture, not in space: it tilts the normal by the difference in
// height between neighbouring texels, times the strength. The same material therefore looks the
// same on a small and on a large object, and a finer texture of the same pattern gives a
// shallower relief. Blender's Bump node works in space, so the same rule is built from nodes:
// the height is read at the shading point and one texel further along U and along V, and the two
// differences become the slopes of a tangent-space normal.
//
// BUMP_SLOPE_PER_TEXEL is measured against the Standard renderer (docs/reference/c4d-sdk.md,
// "Bump"): at 100 % strength a height step of 1 (black to white) between two texels tilts the
// normal by a slope of 7.4.
static const maxon::Float64 BUMP_SLOPE_PER_TEXEL = 7.4;

maxon::Result<NodeOutput> ClassicMaterialGraph::Bump(const maxon::String& image, maxon::Float64 strength)
{
	iferr_scope;

	NodeOutput uv = _shaders.GetCoordinates();
	if (!uv.IsValid())
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}
	const maxon::Int here = _graph.Add("ShaderNodeTexImage") iferr_return;
	_graph.SetProperty(here, "image", image);
	_graph.Link(uv, here, "Vector") iferr_return;

	// The bridge replaces {"image", "is"} by the size of one texel in UV units.
	const maxon::String steps[2] = {
		FormatString("[{\"image\":\"@\",\"is\":\"texel_width\"},0,0]", image),
		FormatString("[0,{\"image\":\"@\",\"is\":\"texel_height\"},0]", image) };
	maxon::Int ahead[2] = { NOTOK, NOTOK };
	for (maxon::Int axis = 0; axis < 2; ++axis)
	{
		const maxon::Int shift = _graph.Add("ShaderNodeMapping") iferr_return;
		_graph.SetProperty(shift, "vector_type", "POINT"_s);
		_graph.SetInputJson(shift, "Location", steps[axis]);
		_graph.Link(uv, shift, "Vector") iferr_return;
		ahead[axis] = _graph.Add("ShaderNodeTexImage") iferr_return;
		_graph.SetProperty(ahead[axis], "image", image);
		_graph.Link(NodeOutput { shift, "Vector"_s }, ahead[axis], "Vector") iferr_return;
	}

	// (height ahead in U, height ahead in V) - (height here, height here)
	const maxon::Int heightsAhead = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	_graph.Link(NodeOutput { ahead[0], "Color"_s }, heightsAhead, "X") iferr_return;
	_graph.Link(NodeOutput { ahead[1], "Color"_s }, heightsAhead, "Y") iferr_return;
	const maxon::Int heightsHere = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	_graph.Link(NodeOutput { here, "Color"_s }, heightsHere, "X") iferr_return;
	_graph.Link(NodeOutput { here, "Color"_s }, heightsHere, "Y") iferr_return;
	const maxon::Int difference = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(difference, "operation", "SUBTRACT"_s);
	_graph.Link(NodeOutput { heightsAhead, "Vector"_s }, difference, "Vector") iferr_return;
	_graph.Link(NodeOutput { heightsHere, "Vector"_s }, difference, "Vector_001") iferr_return;

	// A surface rising along U leans its normal back along U: normal = (-slopeU, -slopeV, 1),
	// written the way a normal map stores it, colour = normal / 2 + 1/2.
	const maxon::Float64 slope = BUMP_SLOPE_PER_TEXEL * strength;
	const maxon::Int encode = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(encode, "operation", "MULTIPLY_ADD"_s);
	_graph.Link(NodeOutput { difference, "Vector"_s }, encode, "Vector") iferr_return;
	_graph.SetInput(encode, "Vector_001", cinema::Vector(-0.5 * slope, -0.5 * slope, 0.0));
	_graph.SetInput(encode, "Vector_002", cinema::Vector(0.5, 0.5, 1.0));

	const maxon::Int map = _graph.Add("ShaderNodeNormalMap") iferr_return;
	_graph.SetProperty(map, "space", "TANGENT"_s);
	_graph.SetInput(map, "Strength", 1.0);
	_graph.Link(NodeOutput { encode, "Vector"_s }, map, "Color") iferr_return;
	return NodeOutput { map, "Normal"_s };
}

maxon::Result<void> ClassicMaterialGraph::BumpAndNormal()
{
	iferr_scope;

	NodeOutput normal;

	if (Uses(MATERIAL_USE_NORMAL))
	{
		const NodeOutput texture = Texture(GetShaderParameter(_material, MATERIAL_NORMAL_SHADER, _exporter.GetDocument()), false, "normal") iferr_return;
		if (texture.IsValid())
		{
			const maxon::Int map = _graph.Add("ShaderNodeNormalMap") iferr_return;
			_graph.SetProperty(map, "space", "TANGENT"_s);
			_graph.SetInput(map, "Strength", GetFloatParameter(_material, MATERIAL_NORMAL_STRENGTH, 1.0));
			_graph.Link(texture, map, "Color") iferr_return;
			normal = NodeOutput { map, "Normal"_s };
		}
	}

	if (Uses(MATERIAL_USE_BUMP))
	{
		cinema::BaseShader* const shader = GetShaderParameter(_material, MATERIAL_BUMP_SHADER, _exporter.GetDocument());
		// The bump graph steps from texel to texel, so it needs an image: the bitmap's file,
		// or the shader baked into one.
		maxon::String image = _exporter.GetImageId(shader, false) iferr_return;
		if (shader && image.IsEmpty() && ShaderTranslator::CanBake(shader))
		{
			image = _exporter.GetBakedImageId(shader) iferr_return;
		}
		if (shader && image.IsEmpty())
		{
			_exporter.GetWriter().Warn(FormatString("Material \"@\": the @ shader in the bump channel is not translated; the surface stays smooth.",
				_material->GetName(), shader->GetTypeName()));
		}
		else if (shader && normal.IsValid())
		{
			NotTranslated("a bump map together with a normal map (the normal map is used)");
		}
		else if (shader)
		{
			normal = Bump(image, GetFloatParameter(_material, MATERIAL_BUMP_STRENGTH, 0.2)) iferr_return;
		}
	}

	if (normal.IsValid())
	{
		_graph.Link(normal, _bsdf, "Normal") iferr_return;
	}
	return maxon::OK;
}

maxon::Result<void> ClassicMaterialGraph::Build()
{
	iferr_scope;

	_bsdf = _graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
	Coordinates() iferr_return;

	// Transparency: the Principled BSDF tints what passes through with its base colour, so the
	// base colour moves towards the transparency colour by the amount of transparency.
	maxon::Float64 transmission = 0.0;
	cinema::Vector transmissionTint(1.0);
	if (Uses(MATERIAL_USE_TRANSPARENCY))
	{
		const cinema::Vector transparency = GetVectorParameter(_material, MATERIAL_TRANSPARENCY_COLOR, cinema::Vector(1.0)) *
			GetFloatParameter(_material, MATERIAL_TRANSPARENCY_BRIGHTNESS, 1.0);
		transmission = maxon::Clamp01(MaxComponent(transparency));
		if (transmission > 1e-6)
			transmissionTint = transparency / MaxComponent(transparency);
		_graph.SetInput(_bsdf, "Transmission Weight", transmission);
		_graph.SetInput(_bsdf, "IOR", maxon::Max(1.0, GetFloatParameter(_material, MATERIAL_TRANSPARENCY_REFRACTION, 1.0)));
		if (GetShaderParameter(_material, MATERIAL_TRANSPARENCY_SHADER, _exporter.GetDocument()))
			NotTranslated("a texture in the transparency channel");
	}

	if (Uses(MATERIAL_USE_COLOR))
	{
		ColorChannel("Base Color", "colour", MATERIAL_COLOR_COLOR, MATERIAL_COLOR_BRIGHTNESS, MATERIAL_COLOR_SHADER,
			MATERIAL_COLOR_TEXTUREMIXING, MATERIAL_COLOR_TEXTURESTRENGTH, transmissionTint, transmission) iferr_return;
	}
	else
	{
		// Without a colour channel the surface has no diffuse reflection.
		_graph.SetInput(_bsdf, "Base Color", transmissionTint * transmission);
	}

	if (Uses(MATERIAL_USE_LUMINANCE))
	{
		ColorChannel("Emission Color", "luminance", MATERIAL_LUMINANCE_COLOR, MATERIAL_LUMINANCE_BRIGHTNESS, MATERIAL_LUMINANCE_SHADER,
			MATERIAL_LUMINANCE_TEXTUREMIXING, MATERIAL_LUMINANCE_TEXTURESTRENGTH, cinema::Vector(0.0), 0.0) iferr_return;
		_graph.SetInput(_bsdf, "Emission Strength", 1.0);
	}

	Reflectance() iferr_return;
	Alpha() iferr_return;
	BumpAndNormal() iferr_return;
	Displacement() iferr_return;
	return maxon::OK;
}

// Cinema 4D moves the surface along its normal by the texture's brightness times the height
// (and the strength), in document units. "Intensity (Centered)" takes mid grey as no movement.
// Blender's Displacement node does the same; what it moves are the points the mesh has
// (Cycles), plus a bump for the detail in between (both engines).
maxon::Result<void> ClassicMaterialGraph::Displacement()
{
	iferr_scope;

	if (!Uses(MATERIAL_USE_DISPLACEMENT))
		return maxon::OK;
	const maxon::Int32 type = GetIntParameter(_material, MATERIAL_DISPLACEMENT_TYPE, MATERIAL_DISPLACEMENT_TYPE_CENTEREDINTENSITY);
	if (type != MATERIAL_DISPLACEMENT_TYPE_INTENSITY && type != MATERIAL_DISPLACEMENT_TYPE_CENTEREDINTENSITY)
	{
		NotTranslated("a displacement by colour (red/green, RGB)");
		return maxon::OK;
	}
	const NodeOutput height = Texture(GetShaderParameter(_material, MATERIAL_DISPLACEMENT_SHADER, _exporter.GetDocument()), false, "displacement") iferr_return;
	if (!height.IsValid())
		return maxon::OK;
	const maxon::Int node = _graph.Add("ShaderNodeDisplacement") iferr_return;
	_graph.SetProperty(node, "space", "OBJECT"_s);
	_graph.Link(height, node, "Height") iferr_return;
	// Measured: "Intensity (Centered)", the default, moves from -height (black) to +height (white).
	const maxon::Bool centered = type == MATERIAL_DISPLACEMENT_TYPE_CENTEREDINTENSITY;
	_graph.SetInput(node, "Midlevel", centered ? 0.5 : 0.0);
	_graph.SetInput(node, "Scale", Coords::FromDocument(_exporter.GetDocument()).Length(GetFloatParameter(_material, MATERIAL_DISPLACEMENT_HEIGHT, 5.0)) *
		GetFloatParameter(_material, MATERIAL_DISPLACEMENT_STRENGTH, 1.0) * (centered ? 2.0 : 1.0));
	_displacement = NodeOutput { node, "Displacement"_s };
	return maxon::OK;
}

// A point of the object in the texture tag's axes, each component then scaled and shifted:
// out[k] = (tag space point)[k] * scale[k] + shift[k], with the components in Cinema 4D's
// order (X, Y up, Z). Blender's object coordinates are Cinema 4D's local ones with Y and Z
// swapped, in metres.
maxon::Result<NodeOutput> ClassicMaterialGraph::TextureSpacePoint(const maxon::Float64 scale[3], const maxon::Float64 shift[3])
{
	iferr_scope;

	const maxon::Float64 unit = Coords::FromDocument(_exporter.GetDocument()).GetScale();
	const cinema::Matrix inverse = ~_mapping.ml;
	// What one metre along Blender's X, Y and Z is in the tag's axes.
	const cinema::Vector alongX = inverse.sqmat * (cinema::Vector(1.0, 0.0, 0.0) / unit);
	const cinema::Vector alongY = inverse.sqmat * (cinema::Vector(0.0, 0.0, 1.0) / unit);
	const cinema::Vector alongZ = inverse.sqmat * (cinema::Vector(0.0, 1.0, 0.0) / unit);

	const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
	const maxon::Int combine = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	const maxon::Char* const sockets[3] = { "X", "Y", "Z" };
	for (maxon::Int k = 0; k < 3; ++k)
	{
		const maxon::Int dot = _graph.Add("ShaderNodeVectorMath") iferr_return;
		_graph.SetProperty(dot, "operation", "DOT_PRODUCT"_s);
		_graph.Link(NodeOutput { coordinates, "Object"_s }, dot, "Vector") iferr_return;
		_graph.SetInput(dot, "Vector_001", cinema::Vector(alongX[k], alongY[k], alongZ[k]) * scale[k]);
		const maxon::Int add = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(add, "operation", "ADD"_s);
		_graph.Link(NodeOutput { dot, "Value"_s }, add, "Value") iferr_return;
		_graph.SetInput(add, "Value_001", inverse.off[k] * scale[k] + shift[k]);
		_graph.Link(NodeOutput { add, "Value"_s }, combine, sockets[k]) iferr_return;
	}
	return NodeOutput { combine, "Vector"_s };
}

// Where the textures of this material are looked up: the texture tag's projection, then its
// offset and length (tiles). The result is a UV pair in Blender's convention (V up).
//
//   UVW          the mesh's UV coordinates
//   flat         the tag's XY plane: -1..1 becomes 0..1
//   cylindrical  around the tag's Y axis; height -1..1 becomes 0..1
//   spherical    around the tag's Y axis; pole to pole
//   cubic        flat from six sides (Blender's box projection of images)
maxon::Result<void> ClassicMaterialGraph::Coordinates()
{
	iferr_scope;

	if (_mapping.IsPlain())
		return maxon::OK;

	const maxon::Float64 lengthX = maxon::Abs(_mapping.lengthX) > 1e-9 ? _mapping.lengthX : 1.0;
	const maxon::Float64 lengthY = maxon::Abs(_mapping.lengthY) > 1e-9 ? _mapping.lengthY : 1.0;
	NodeOutput uv;
	maxon::Bool box = false;

	if (_mapping.projection == TEXTURETAG_PROJECTION_UVW)
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}
	else if (_mapping.projection == TEXTURETAG_PROJECTION_FLAT)
	{
		const maxon::Float64 scale[3] = { 0.5, 0.5, 0.0 };
		const maxon::Float64 shift[3] = { 0.5, 0.5, 0.0 };
		uv = TextureSpacePoint(scale, shift) iferr_return;
	}
	else if (_mapping.projection == TEXTURETAG_PROJECTION_CUBIC)
	{
		// The cube's sides are 2 units of the tag's axes; tiles shrink them around the centre.
		const maxon::Float64 scale[3] = { 0.5 / lengthX, 0.5 / lengthY, 0.5 / lengthX };
		const maxon::Float64 shift[3] = { 0.5, 0.5, 0.5 };
		const NodeOutput point = TextureSpacePoint(scale, shift) iferr_return;
		// Back to Blender's order of axes, which is what its box projection goes by.
		const maxon::Int separate = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
		_graph.Link(point, separate, "Vector") iferr_return;
		const maxon::Int combine = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
		_graph.Link(NodeOutput { separate, "X"_s }, combine, "X") iferr_return;
		_graph.Link(NodeOutput { separate, "Z"_s }, combine, "Y") iferr_return;
		_graph.Link(NodeOutput { separate, "Y"_s }, combine, "Z") iferr_return;
		_shaders.SetCoordinates(NodeOutput { combine, "Vector"_s }, true, !_mapping.tile);
		if (_mapping.offsetX != 0.0 || _mapping.offsetY != 0.0)
			NotTranslated("the offset of a cubic projection");
		return maxon::OK;
	}
	else if (_mapping.projection == TEXTURETAG_PROJECTION_CYLINDRICAL || _mapping.projection == TEXTURETAG_PROJECTION_SPHERICAL)
	{
		const maxon::Float64 scale[3] = { 1.0, 1.0, 1.0 };
		const maxon::Float64 shift[3] = { 0.0, 0.0, 0.0 };
		const NodeOutput point = TextureSpacePoint(scale, shift) iferr_return;
		const maxon::Int separate = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
		_graph.Link(point, separate, "Vector") iferr_return;

		// U: the angle around the Y axis, as a fraction of the full turn. Measured against the
		// Standard renderer: U is 0 towards +X and grows towards -Z (the side facing a default camera).
		const maxon::Int angle = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(angle, "operation", "ARCTAN2"_s);
		_graph.Link(NodeOutput { separate, "Z"_s }, angle, "Value") iferr_return;
		_graph.Link(NodeOutput { separate, "X"_s }, angle, "Value_001") iferr_return;
		const maxon::Int turn = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(turn, "operation", "MULTIPLY"_s);
		_graph.Link(NodeOutput { angle, "Value"_s }, turn, "Value") iferr_return;
		_graph.SetInput(turn, "Value_001", SPHERICAL_U_DIRECTION / maxon::PI2);
		const maxon::Int shifted = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(shifted, "operation", "ADD"_s);
		_graph.Link(NodeOutput { turn, "Value"_s }, shifted, "Value") iferr_return;
		_graph.SetInput(shifted, "Value_001", SPHERICAL_U_START);
		const maxon::Int wrap = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(wrap, "operation", "FRACT"_s);
		_graph.Link(NodeOutput { shifted, "Value"_s }, wrap, "Value") iferr_return;

		NodeOutput v;
		if (_mapping.projection == TEXTURETAG_PROJECTION_CYLINDRICAL)
		{
			const maxon::Int height = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(height, "operation", "MULTIPLY_ADD"_s);
			_graph.Link(NodeOutput { separate, "Y"_s }, height, "Value") iferr_return;
			_graph.SetInput(height, "Value_001", 0.5);
			_graph.SetInput(height, "Value_002", 0.5);
			v = NodeOutput { height, "Value"_s };
		}
		else
		{
			// V up = 1 - (angle from the +Y pole) / pi = 0.5 + asin(y / length) / pi
			const maxon::Int length = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(length, "operation", "LENGTH"_s);
			_graph.Link(point, length, "Vector") iferr_return;
			const maxon::Int ratio = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(ratio, "operation", "DIVIDE"_s);
			_graph.Link(NodeOutput { separate, "Y"_s }, ratio, "Value") iferr_return;
			_graph.Link(NodeOutput { length, "Value"_s }, ratio, "Value_001") iferr_return;
			const maxon::Int latitude = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(latitude, "operation", "ARCSINE"_s);
			_graph.Link(NodeOutput { ratio, "Value"_s }, latitude, "Value") iferr_return;
			const maxon::Int height = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(height, "operation", "MULTIPLY_ADD"_s);
			_graph.Link(NodeOutput { latitude, "Value"_s }, height, "Value") iferr_return;
			_graph.SetInput(height, "Value_001", 1.0 / maxon::PI);
			_graph.SetInput(height, "Value_002", 0.5);
			v = NodeOutput { height, "Value"_s };
		}
		const maxon::Int combine = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
		_graph.Link(NodeOutput { wrap, "Value"_s }, combine, "X") iferr_return;
		_graph.Link(v, combine, "Y") iferr_return;
		uv = NodeOutput { combine, "Vector"_s };
	}
	else
	{
		NotTranslated("this projection of its texture tag (UVW mapping is used)");
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}

	// Offset and length are given with V running down: u' = (u - offsetX) / lengthX and the
	// same for v. Written for Blender's V, which runs up: V' = V / lengthY + 1 - (1 - offsetY) / lengthY.
	if (_mapping.offsetX != 0.0 || _mapping.offsetY != 0.0 || lengthX != 1.0 || lengthY != 1.0)
	{
		const maxon::Int place = _graph.Add("ShaderNodeVectorMath") iferr_return;
		_graph.SetProperty(place, "operation", "MULTIPLY_ADD"_s);
		_graph.Link(uv, place, "Vector") iferr_return;
		_graph.SetInput(place, "Vector_001", cinema::Vector(1.0 / lengthX, 1.0 / lengthY, 1.0));
		_graph.SetInput(place, "Vector_002", cinema::Vector(-_mapping.offsetX / lengthX, 1.0 - (1.0 - _mapping.offsetY) / lengthY, 0.0));
		uv = NodeOutput { place, "Vector"_s };
	}
	_shaders.SetCoordinates(uv, box, !_mapping.tile);
	return maxon::OK;
}

//----------------------------------------------------------------------------------------

maxon::Result<maxon::String> MaterialExporter::GetImageId(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	if (!shader || shader->GetType() != Xbitmap)
		return maxon::String();
	const cinema::Filename name = GetFilenameParameter(shader, BITMAPSHADER_FILENAME);
	if (!name.IsPopulated())
		return maxon::String();

	// Resolve the name the way Cinema 4D does: next to the document, in its "tex" folder, in
	// the texture search paths. An unresolved name is sent as it is; the bridge reports it.
	cinema::Filename resolved;
	if (!cinema::GenerateTexturePath(_doc ? _doc->GetDocumentPath() : cinema::Filename(), name, cinema::Filename(), &resolved) || !resolved.IsPopulated())
		resolved = name;

	// With "Basic" colour management the bitmap's own colour profile says what its values are.
	const maxon::Int32 profile = GetIntParameter(shader, BITMAPSHADER_COLORPROFILE, BITMAPSHADER_COLORPROFILE_EMBEDDED);
	if (colorData && !_colors.ImageColorSpace(IsLinearFile(resolved), profile))
		return maxon::String();
	return GetFileImageId(resolved.GetString(), colorData, profile);
}

maxon::Result<maxon::String> MaterialExporter::GetPixelImageId(const maxon::BaseArray<maxon::Vector4d32>& pixels, maxon::Int32 width, maxon::Int32 height)
{
	iferr_scope;

	if (width < 1 || height < 1 || pixels.GetCount() < maxon::Int(width) * height)
		return maxon::String();
	const maxon::UInt64 hash = HashBytes(reinterpret_cast<const maxon::Char*>(pixels.GetFirst()), pixels.GetCount() * SIZEOF(maxon::Vector4d32),
		maxon::UInt64(width) * 0x9E3779B97F4A7C15ull + maxon::UInt64(height));
	const maxon::String id = FormatString("image_pixels_@", maxon::String::HexToString(hash, false));
	if (_images.Find(id))
		return id;

	// The same pixels written before in this session are written again only if their file is gone.
	maxon::String path = FindBakedFile(id, _doc, _writer) iferr_return;
	if (path.IsEmpty())
	{
		const cinema::Filename file = NewBakedFileName(id);
		if (!file.IsPopulated())
			return maxon::String();
		cinema::AutoAlloc<cinema::BaseBitmap> bitmap;
		if (!bitmap || bitmap->Init(width, height, 96) != cinema::IMAGERESULT::OK)
			return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
		for (maxon::Int32 y = 0; y < height; ++y)
		{
			// 16 bytes from one pixel to the next: the alpha value is skipped.
			bitmap->SetPixelCnt(0, y, width, reinterpret_cast<maxon::UChar*>(const_cast<maxon::Vector4d32*>(pixels.GetFirst() + maxon::Int(y) * width)),
				16, cinema::COLORMODE::RGBf, cinema::PIXELCNT::NONE);
		}
		if (bitmap->Save(file, FILTER_EXR, nullptr, cinema::SAVEBIT::USE32BITCHANNELS) != cinema::IMAGERESULT::OK)
		{
			cinema::GeFKill(file);   // what was written before saving failed; the name is this session's own
			return maxon::String();
		}
		path = AddBakedFile(id, file, _doc, _writer) iferr_return;
	}
	_images.Insert(id, id) iferr_return;
	JsonWriter& json = _writer.BeginItem("image", id);
	json.String("path", path);
	json.String("colorspace", maxon::String(COLORSPACE_DATA));
	_writer.EndItem();
	return id;
}

maxon::Result<maxon::String> MaterialExporter::GetFileImageId(const maxon::String& path, maxon::Bool colorData)
{
	iferr_scope;

	const cinema::Filename resolved(path);
	const maxon::Char* colorspace = colorData ? _colors.ImageColorSpace(IsLinearFile(resolved), bitmapProfile) : COLORSPACE_DATA;
	if (!colorspace)
	{
		// A file without a bitmap shader to bake (a node material's, a Redshift dome's).
		colorspace = COLORSPACE_LINEAR_COLOR;
		_writer.Warn(FormatString("The image \"@\" holds linear values; with Linear Workflow off Cinema 4D renders them with the sRGB curve "
			"applied, which Blender cannot do when it loads the file. Its values are used as they are, so it renders darker.", resolved.GetFileString()));
	}
	const maxon::String key = path + "|"_s + maxon::String(colorspace);
	if (const auto* const known = _images.Find(key))
		return known->GetValue();

	// The id follows from the file and its colour space, so a material whose texture changes
	// refers to a different image and is sent again.
	const maxon::String id = FormatString("image_@", maxon::String::HexToString(maxon::UInt64(key.GetHashCode()), false));
	_images.Insert(key, id) iferr_return;
	JsonWriter& json = _writer.BeginItem("image", id);
	json.String("path", path);
	json.String("colorspace", maxon::String(colorspace));
	_writer.EndItem();
	return id;
}

// Shaders baked by this process, for tests. Exports run on the main thread (IPR) and on
// render threads, hence the lock.
static maxon::Spinlock g_bakeLock;
static maxon::Int g_bakeCount = 0;

maxon::Int MaterialExporter::GetBakeCount()
{
	maxon::ScopedLock lock(g_bakeLock);
	return g_bakeCount;
}

// A fingerprint of the settings of the shader and of every shader inside it. It is made from
// the settings themselves, as Cinema 4D would save them: the same shader in the copy of the
// document that a render works on gives the same fingerprint, and two shaders of one kind in
// one material do not. (Dirty counters fail on both counts.) A bitmap shader's picture counts
// with the time its file was last changed, so that a texture edited on disk is baked again.
static maxon::UInt64 ShaderFingerprint(cinema::BaseShader* shader, const cinema::BaseDocument* doc, maxon::UInt64 hash)
{
	hash = (hash ^ maxon::UInt64(shader->GetType())) * 0x100000001B3ull;
	cinema::AutoAlloc<cinema::HyperFile> file;
	cinema::AutoAlloc<cinema::MemoryFileStruct> memory;
	if (file && memory)
	{
		cinema::Filename name;
		name.SetMemoryWriteMode(memory);
		if (file->Open(0, name, cinema::FILEOPEN::WRITE, cinema::FILEDIALOG::NONE))
		{
			file->WriteContainer(shader->GetDataInstanceRef());
			file->Close();
			void* data = nullptr;
			maxon::Int size = 0;
			memory->GetData(data, size, false);
			if (data && size > 0)
				hash = HashBytes(static_cast<const maxon::Char*>(data), size, hash);
		}
	}
	for (cinema::BaseShader* first : { shader->GetFirstShader(), shader->GetDown() })
	{
		for (cinema::BaseShader* child = first; child; child = child->GetNext())
			hash = ShaderFingerprint(child, doc, hash);
	}
	return hash;
}

//----------------------------------------------------------------------------------------
// True when the shader or one inside it gives another picture at another time with the same
// settings: it says so itself (SHADERINFO::TIMEDEPENDENT, which the Noise shader reports when
// its animation speed is not 0, measured), or it is a bitmap shader playing a movie or an
// image sequence. A shader animated by keys needs no such care: its settings change, and with
// them the fingerprint.
//----------------------------------------------------------------------------------------
static maxon::Bool IsTimeDependent(cinema::BaseShader* shader)
{
	if (shader->GetRenderInfo() & cinema::SHADERINFO::TIMEDEPENDENT)
		return true;
	if (shader->GetType() == Xbitmap &&
		GetIntParameter(shader, BITMAPSHADER_TIMING_TO, 0) != GetIntParameter(shader, BITMAPSHADER_TIMING_FROM, 0))
		return true;
	for (cinema::BaseShader* first : { shader->GetFirstShader(), shader->GetDown() })
	{
		for (cinema::BaseShader* child = first; child; child = child->GetNext())
		{
			if (IsTimeDependent(child))
				return true;
		}
	}
	return false;
}

maxon::Result<maxon::String> MaterialExporter::GetBakedImageId(cinema::BaseShader* shader)
{
	iferr_scope;

	if (!shader || !_doc)
		return maxon::String();

	// What the picture depends on besides the shaders' settings. The owner's name: two
	// materials may hold shaders that look alike to the fingerprint but differ in what they
	// link to. The document's colour settings, which decide the values baked. The frame, only
	// when the shader changes over time by itself: otherwise every frame of an animation would
	// be baked again.
	const cinema::BaseList2D* const owner = shader->GetMain();
	const cinema::BaseContainer& documentData = _doc->GetDataInstanceRef();
	const maxon::Int32 colorProfile = documentData.GetInt32(cinema::DOCUMENT_COLORPROFILE, cinema::DOCUMENT_COLORPROFILE_SRGB);
	const maxon::Bool linearWorkflow = documentData.GetBool(cinema::DOCUMENT_LINEARWORKFLOW, true);
	maxon::UInt64 key = ShaderFingerprint(shader, _doc, maxon::UInt64(_bakeResolution) * 0x9E3779B97F4A7C15ull);
	key = (key ^ maxon::UInt64(owner ? owner->GetName().GetHashCode() : 0)) * 0x100000001B3ull;
	key = (key ^ maxon::UInt64(colorProfile) ^ (maxon::UInt64(linearWorkflow) << 8) ^
		(maxon::UInt64(documentData.GetInt32(cinema::DOCUMENT_COLOR_MANAGEMENT)) << 16) ^
		(maxon::UInt64(documentData.GetInt32(cinema::DOCUMENT_OCIO_RENDER_COLORSPACE)) << 24)) * 0x100000001B3ull;
	key = (key ^ maxon::UInt64(documentData.GetFilename(cinema::DOCUMENT_OCIO_CONFIG).GetString().GetHashCode())) * 0x100000001B3ull;
	if (IsTimeDependent(shader))
		key = (key ^ maxon::UInt64(_doc->GetTime().GetFrame(_doc->GetFps()) + 1)) * 0x100000001B3ull;
	const maxon::String id = FormatString("image_baked_@", maxon::String::HexToString(key, false));
	if (_images.Find(id))
		return id;

	// Baked before in this session (baked_files.h), or now.
	maxon::String path = FindBakedFile(id, _doc, _writer) iferr_return;
	if (path.IsEmpty())
	{
		const cinema::Filename file = NewBakedFileName(id);
		if (!file.IsPopulated())
		{
			_writer.Warn("The folder for baked shaders could not be created in Cinema 4D's preferences folder."_s);
			return maxon::String();
		}

		cinema::AutoAlloc<cinema::BaseBitmap> bitmap;
		if (!bitmap || bitmap->Init(_bakeResolution, _bakeResolution, 96) != cinema::IMAGERESULT::OK)
			return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
		const maxon::Bool baked = shader->BakeShaderIntoBaseBitmap(*bitmap, *_doc, nullptr, false, colorProfile, linearWorkflow,
			true, 0, _bakeResolution - 1, 0, _bakeResolution - 1);
		if (!baked || bitmap->Save(file, FILTER_EXR, nullptr, cinema::SAVEBIT::USE32BITCHANNELS) != cinema::IMAGERESULT::OK)
		{
			cinema::GeFKill(file);   // what was written before saving failed; the name is this session's own
			_writer.Warn(FormatString("The @ shader \"@\" could not be baked into a texture.", shader->GetTypeName(), shader->GetName()));
			return maxon::String();
		}
		path = AddBakedFile(id, file, _doc, _writer) iferr_return;

		maxon::ScopedLock lock(g_bakeLock);
		++g_bakeCount;
	}

	_images.Insert(id, id) iferr_return;
	// Baked values are already what Cinema 4D calculates with (the render space, or with
	// "Basic" colour management the space Linear Workflow says): no conversion on loading.
	JsonWriter& json = _writer.BeginItem("image", id);
	json.String("path", path);
	json.String("colorspace", maxon::String(COLORSPACE_DATA));
	_writer.EndItem();
	return id;
}

maxon::Result<void> MaterialExporter::WritePlainMaterial(const maxon::String& id, const cinema::Vector& color, maxon::Float64 specular)
{
	iferr_scope;

	NodeGraph graph;
	const maxon::Int bsdf = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	graph.SetInput(bsdf, "Base Color", color);
	graph.SetInput(bsdf, "Roughness", 0.5);
	graph.SetInput(bsdf, "Specular IOR Level", specular);
	graph.Link(NodeOutput { bsdf, "BSDF"_s }, output, "Surface") iferr_return;

	JsonWriter& json = _writer.BeginItem("material", id);
	graph.Write(json);
	_writer.EndItem();
	return maxon::OK;
}

maxon::Bool MaterialExporter::ImageHasAlpha(cinema::BaseShader* shader)
{
	const cinema::Filename name = GetFilenameParameter(shader, BITMAPSHADER_FILENAME);
	cinema::Filename resolved;
	if (!cinema::GenerateTexturePath(_doc ? _doc->GetDocumentPath() : cinema::Filename(), name, cinema::Filename(), &resolved) || !resolved.IsPopulated())
		resolved = name;
	const maxon::String path = resolved.GetString();
	if (const auto* const known = _imageAlpha.Find(path))
		return known->GetValue();
	// The file has to be opened to know. A file that cannot be read counts as having none.
	cinema::AutoAlloc<cinema::BaseBitmap> bitmap;
	const maxon::Bool hasAlpha = bitmap && bitmap->Init(resolved) == cinema::IMAGERESULT::OK && bitmap->GetInternalChannel() != nullptr;
	_imageAlpha.Insert(path, hasAlpha) iferr_ignore("It is then looked up again next time.");
	return hasAlpha;
}

// A node material has the type of a classic material; what tells them apart is a node graph
// in one of the node spaces.
static maxon::Bool IsNodeMaterial(const cinema::BaseMaterial* material)
{
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	return nodeMaterial->HasSpace(maxon::Id("net.maxon.nodespace.standard")) || nodeMaterial->HasSpace(maxon::Id("com.redshift3d.redshift4c4d.class.nodespace")) ||
		nodeMaterial->HasSpace(maxon::Id(CYCLES_NODESPACE_ID));
}

maxon::Bool MaterialExporter::HasAlpha(cinema::BaseMaterial* material)
{
	return material && material->GetType() == Mmaterial && !IsNodeMaterial(material) && GetBoolParameter(material, MATERIAL_USE_ALPHA, false) &&
		GetParameterData(material, MATERIAL_ALPHA_SHADER).GetLink(material->GetDocument()) != nullptr;
}

maxon::Result<void> MaterialExporter::WriteLayers(const maxon::BaseArray<MaterialLayer>& layers, const maxon::String& id)
{
	iferr_scope;

	// The image items the graph needs are written while it is built, before the material item.
	NodeGraph graph;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	NodeOutput surface;
	NodeOutput displacement;
	CyclesMaterialSettings settings;
	for (maxon::Int i = 0; i < layers.GetCount(); ++i)
	{
		cinema::BaseMaterial* const material = layers[i].material;
		NodeOutput layerSurface;
		NodeOutput layerAlpha;
		if (material->GetType() == Mmaterial && !IsNodeMaterial(material))
		{
			ClassicMaterialGraph classic(*this, graph, static_cast<cinema::Material*>(material), layers[i].mapping);
			classic.Build() iferr_return;
			layerSurface = classic.GetSurface();
			if (surface.IsValid())
			{
				layerAlpha = classic.GetAlpha();
			}
			else
			{
				classic.ApplyAlphaToSurface() iferr_return;
			}
			if (classic.GetDisplacement().IsValid())
				displacement = classic.GetDisplacement();
		}
		else
		{
			// A Redshift material is read from its Standard Material node; any other node
			// material, as Cinema 4D describes it to other programs.
			// A material of Cycles nodes goes to Blender as it is.
			NodeOutput layerDisplacement;
			CyclesMaterialSettings layerSettings;
			maxon::Bool described = BuildCyclesNodeSurface(*this, graph, material, layerSurface, layerDisplacement, layerSettings) iferr_return;
			if (described)
				settings = std::move(layerSettings);   // the topmost layer's count
			if (described && layerDisplacement.IsValid())
				displacement = layerDisplacement;
			if (described && !layerSurface.IsValid())
			{
				// Only a displacement is wired in: the surface shows nothing.
				const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
				layerSurface = NodeOutput { transparent, "BSDF"_s };
			}
			if (!described)
			{
				described = BuildRedshiftSurface(*this, graph, material, layerSurface) iferr_return;
			}
			if (!described)
			{
				described = BuildExchangeSurface(*this, graph, material, layerSurface) iferr_return;
			}
			if (!described)
			{
				Warn(FormatString("Material \"@\": this @ material could not be translated; a plain material of its average colour is used.",
					material->GetName(), material->GetTypeName()));
				const maxon::Int bsdf = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
				graph.SetInput(bsdf, "Base Color", material->GetAverageColor());
				graph.SetInput(bsdf, "Roughness", 0.5);
				graph.SetInput(bsdf, "Specular IOR Level", 0.5);
				layerSurface = NodeOutput { bsdf, "BSDF"_s };
			}
		}

		if (surface.IsValid() && layerAlpha.IsValid())
		{
			// The material below shows where this one's alpha is 0.
			const maxon::Int mix = graph.Add("ShaderNodeMixShader") iferr_return;
			graph.Link(layerAlpha, mix, "Fac") iferr_return;
			graph.Link(surface, mix, 1) iferr_return;
			graph.Link(layerSurface, mix, 2) iferr_return;
			surface = NodeOutput { mix, "Shader"_s };
		}
		else
		{
			surface = layerSurface;
		}
	}
	graph.Link(surface, output, "Surface") iferr_return;
	if (displacement.IsValid())
	{
		graph.Link(displacement, output, "Displacement") iferr_return;
	}

	JsonWriter& json = _writer.BeginItem("material", id);
	graph.Write(json);
	// A material of Cycles nodes brings Blender's own material settings.
	if (!settings.names.IsEmpty())
	{
		json.BeginObject("props");
		for (maxon::Int i = 0; i < settings.names.GetCount(); ++i)
			json.Raw(settings.names[i], settings.values[i]);
		json.EndObject();
	}
	else if (displacement.IsValid())
	{
		// Move the mesh's points, and add a bump for what lies between them.
		json.BeginObject("props");
		json.String("displacement_method", "BOTH"_s);
		json.EndObject();
	}
	_writer.EndItem();
	return maxon::OK;
}

void MaterialExporter::Warn(const maxon::String& message)
{
	_writer.Warn(message);
}

maxon::Result<maxon::String> MaterialExporter::GetMaterialId(cinema::BaseMaterial* material)
{
	iferr_scope;

	maxon::BaseArray<MaterialLayer> layers;
	if (material)
	{
		MaterialLayer layer;
		layer.material = material;
		layers.Append(layer) iferr_return;
	}
	return GetMaterialId(layers);
}

maxon::Result<maxon::String> MaterialExporter::GetMaterialId(const maxon::BaseArray<MaterialLayer>& layers)
{
	iferr_scope;

	if (layers.IsEmpty())
	{
		if (_defaultId.IsEmpty())
		{
			// What Cinema 4D renders for an object without a material: its 80 % grey, which is
			// 0.604 in linear light, with a soft highlight.
			_defaultId = "material_default"_s;
			WritePlainMaterial(_defaultId, cinema::Vector(0.604), 0.5) iferr_return;
		}
		return _defaultId;
	}

	maxon::String key;
	for (const MaterialLayer& layer : layers)
		key += FormatString("@:@;", maxon::String::HexToString(maxon::UInt64(reinterpret_cast<maxon::UInt>(layer.material)), false), layer.mapping.GetKey());
	if (const auto* const known = _materials.Find(key))
		return known->GetValue();

	// A Cycles Native Material is a Blender material already: its bytes go as they are, under
	// an id that stays the same from export to export, because Blender's Shader Editor may be
	// open on that very material (bridge/c4d_bridge/scene/native.py).
	if (layers.GetCount() == 1 && layers[0].mapping.IsPlain())
	{
		if (const NativeMaterial* const native = NativeMaterial::Get(layers[0].material))
		{
			maxon::String id = native->GetItemId();
			// Two materials with one key (a copy that was never opened in the editor): the later one gets a name of its own.
			for (const auto& known : _materials)
			{
				if (known.GetValue() == id)
				{
					id += FormatString("_@", _materials.GetCount());
					break;
				}
			}
			_materials.Insert(key, id) iferr_return;
			const maxon::BaseArray<maxon::Char>& blend = native->GetBlend();
			maxon::Int blob = NOTOK;
			if (!blend.IsEmpty())
			{
				maxon::Char* const bytes = _writer.AllocBytes(blend.GetCount(), blob) iferr_return;
				maxon::MemCopyType(bytes, blend.GetFirst(), blend.GetCount());
			}
			JsonWriter& json = _writer.BeginItem("material", id);
			json.Bool("native", true);
			json.String("name", layers[0].material->GetName());
			if (blob != NOTOK)
				json.Int("blend", blob);
			_writer.EndItem();
			return id;
		}
	}

	const maxon::String id = FormatString("material_@", _materials.GetCount());
	_materials.Insert(key, id) iferr_return;
	WriteLayers(layers, id) iferr_return;
	return id;
}

} // namespace cyclesrender
