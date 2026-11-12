// SPDX-License-Identifier: Apache-2.0
// Cinema 4D shaders as Blender nodes, or baked to a texture (shader_translate.h).
// To translate another shader into nodes, add a case to ShaderTranslator::Native().

#include "c4d.h"
#include "customgui_gradient.h"
#include "lib_layershader.h"
#include "xbitmap.h"
#include "xcheckerboard.h"
#include "xcolor.h"
#include "xmg_color.h"
#include "xslacolorizer.h"
#include "xslafresnel.h"
#include "xslafusion.h"
#include "xslagradient.h"
#include "xslavertexmap.h"

#include "../scene/parameters.h"
#include "material_export.h"
#include "shader_translate.h"

namespace cyclesrender
{

// A Color Ramp holds at most this many stops.
static const maxon::Int MAX_RAMP_STOPS = 32;

// Shaders whose result depends on more than the UV coordinates: the view, the mesh, the scene.
// A tree containing one of them cannot be baked into a texture.
static const maxon::Int32 NOT_BAKEABLE[] = { Xfresnel, Xvertexmap, Xmgcolor, Xmgmultishader, Xfalloff, Xlumas, Xproximal, Xnormaldirection,
	Xambientocclusion, Xsss, Xthinfilm, Xspectral, Xterrainmask };

maxon::Bool ShaderTranslator::CanBake(cinema::BaseShader* shader)
{
	if (!shader)
		return true;
	for (maxon::Int32 type : NOT_BAKEABLE)
	{
		if (shader->GetType() == type)
			return false;
	}
	// The shaders inside a shader are in its shader list, or below it in the hierarchy.
	for (cinema::BaseShader* first : { shader->GetFirstShader(), shader->GetDown() })
	{
		for (cinema::BaseShader* child = first; child; child = child->GetNext())
		{
			if (!CanBake(child))
				return false;
		}
	}
	return true;
}

maxon::Result<NodeOutput> ShaderTranslator::Translate(cinema::BaseShader* shader, maxon::Bool colorData, const maxon::Char* channel)
{
	iferr_scope;

	if (!shader)
		return NodeOutput();
	const NodeOutput result = Any(shader, colorData) iferr_return;
	if (!result.IsValid())
	{
		_exporter.GetWriter().Warn(FormatString("Material \"@\": the @ shader in the @ channel is not translated; the channel's plain value is used.",
			_materialName, shader->GetTypeName(), channel));
	}
	return result;
}

maxon::Result<NodeOutput> ShaderTranslator::Any(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	if (!shader)
		return NodeOutput();
	const NodeOutput native = Native(shader, colorData) iferr_return;
	if (native.IsValid() || !CanBake(shader))
		return native;
	const maxon::String image = _exporter.GetBakedImageId(shader) iferr_return;
	if (image.IsEmpty())
		return NodeOutput();
	return Image(image, false, "Color");
}

maxon::Result<NodeOutput> ShaderTranslator::Native(cinema::BaseShader* shader, maxon::Bool colorData)
{
	switch (shader->GetType())
	{
		case Xbitmap: return Bitmap(shader, colorData);
		case Xcolor: return PlainColor(shader);
		case Xcheckerboard: return Checkerboard(shader);
		case Xgradient: return GradientShader(shader);
		case Xcolorizer: return Colorizer(shader, colorData);
		case Xfresnel: return Fresnel(shader);
		case Xfusion: return Fusion(shader, colorData);
		case Xlayer: return Layer(shader, colorData);
		case Xvertexmap:
		case Xmgcolor: return MeshAttribute(shader);
		default: return NodeOutput();
	}
}

maxon::Result<NodeOutput> ShaderTranslator::Image(const maxon::String& imageId, maxon::Bool closest, const maxon::Char* output)
{
	iferr_scope;

	const maxon::Int node = _graph.Add("ShaderNodeTexImage") iferr_return;
	_graph.SetProperty(node, "image", imageId);
	if (closest)
		_graph.SetProperty(node, "interpolation", "Closest"_s);
	if (_box)
		_graph.SetProperty(node, "projection", "BOX"_s);
	if (_clip)
		_graph.SetProperty(node, "extension", "CLIP"_s);
	if (_uv.IsValid())
	{
		_graph.Link(_uv, node, "Vector") iferr_return;
	}
	return NodeOutput { node, maxon::String(output) };
}

maxon::Result<NodeOutput> ShaderTranslator::Bitmap(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	// Exposure, gamma, black and white point change the pixels; baking applies them exactly.
	if (GetFloatParameter(shader, BITMAPSHADER_EXPOSURE, 0.0) != 0.0 || GetFloatParameter(shader, BITMAPSHADER_GAMMA, 1.0) != 1.0 ||
		GetFloatParameter(shader, BITMAPSHADER_BLACKPOINT, 0.0) != 0.0 || GetFloatParameter(shader, BITMAPSHADER_WHITEPOINT, 1.0) != 1.0)
		return NodeOutput();
	const maxon::String image = _exporter.GetImageId(shader, colorData) iferr_return;
	if (image.IsEmpty())
		return NodeOutput();
	return Image(image, GetIntParameter(shader, BITMAPSHADER_INTERPOLATION, BITMAPSHADER_INTERPOLATION_MIP) == BITMAPSHADER_INTERPOLATION_NONE, "Color");
}

maxon::Result<NodeOutput> ShaderTranslator::PlainColor(cinema::BaseShader* shader)
{
	iferr_scope;

	const cinema::Vector color = _exporter.GetColors().Input(GetVectorParameter(shader, COLORSHADER_COLOR, cinema::Vector(1.0)) * GetFloatParameter(shader, COLORSHADER_BRIGHTNESS, 1.0));
	const maxon::Int node = _graph.Add("ShaderNodeCombineColor") iferr_return;
	_graph.SetInput(node, "Red", color.x);
	_graph.SetInput(node, "Green", color.y);
	_graph.SetInput(node, "Blue", color.z);
	return NodeOutput { node, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Checkerboard(cinema::BaseShader* shader)
{
	iferr_scope;

	// A frequency of 1 is one pair of fields, so two fields, across the UV square. Blender's
	// Checker Texture makes fields of size 1; the coordinates are scaled to fit.
	const maxon::Float64 frequencyU = GetFloatParameter(shader, CHECKERBOARDSHADER_SCALEX, 1.0);
	const maxon::Float64 frequencyV = GetFloatParameter(shader, CHECKERBOARDSHADER_SCALEY, 1.0);
	NodeOutput uv = _uv;
	if (!uv.IsValid())
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}
	// Cinema 4D counts the fields from the top of the UV square, Blender from the bottom:
	// V is turned over (1 - V) before it is scaled.
	const maxon::Int scale = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(scale, "operation", "MULTIPLY_ADD"_s);
	_graph.Link(uv, scale, "Vector") iferr_return;
	_graph.SetInput(scale, "Vector_001", cinema::Vector(2.0 * frequencyU, -2.0 * frequencyV, 1.0));
	_graph.SetInput(scale, "Vector_002", cinema::Vector(0.0, 2.0 * frequencyV, 0.0));
	const maxon::Int checker = _graph.Add("ShaderNodeTexChecker") iferr_return;
	_graph.Link(NodeOutput { scale, "Vector"_s }, checker, "Vector") iferr_return;
	_graph.SetInput(checker, "Scale", 1.0);
	// Measured: the field at the top left shows Cinema 4D's first colour, and Blender's second.
	const DocumentColors& colors = _exporter.GetColors();
	_graph.SetInput(checker, "Color2", colors.Input(GetVectorParameter(shader, CHECKERBOARDSHADER_COLOR1, cinema::Vector(0.0))));
	_graph.SetInput(checker, "Color1", colors.Input(GetVectorParameter(shader, CHECKERBOARDSHADER_COLOR2, cinema::Vector(1.0))));
	return NodeOutput { checker, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::ImageAlpha(cinema::BaseShader* shader)
{
	iferr_scope;

	if (!shader || shader->GetType() != Xbitmap)
		return NodeOutput();
	const maxon::String image = _exporter.GetImageId(shader, false) iferr_return;
	if (image.IsEmpty())
		return NodeOutput();
	return Image(image, false, "Alpha");
}

maxon::Result<NodeOutput> ShaderTranslator::MeshAttribute(cinema::BaseShader* shader)
{
	iferr_scope;

	// Data of the mesh, found by name: the mesh exporter names vertex maps and vertex colours
	// after their tags.
	if (shader->GetType() == Xvertexmap)
	{
		const cinema::GeData link = GetParameterData(shader, SLA_DIRTY_VMAP_OBJECT);
		const cinema::BaseList2D* const tag = link.GetLink(_exporter.GetDocument());
		if (!tag || !(tag->IsInstanceOf(Tvertexmap) || tag->IsInstanceOf(Tvertexcolor)))
			return NodeOutput();
		const maxon::Int node = _graph.Add("ShaderNodeAttribute") iferr_return;
		_graph.SetProperty(node, "attribute_type", "GEOMETRY"_s);
		_graph.SetProperty(node, "attribute_name", maxon::String(tag->GetName()));
		const NodeOutput value { node, "Color"_s };
		if (GetBoolParameter(shader, SLA_DIRTY_VMAP_INVERT, false))
			return Invert(value);
		return value;
	}
	// The colour MoGraph gives each clone. Clones are sent as instancers carrying it.
	if (GetIntParameter(shader, MGCOLORSHADER_MODE, MGCOLORSHADER_MODE_COLOR) != MGCOLORSHADER_MODE_COLOR)
		return NodeOutput();
	const maxon::Int node = _graph.Add("ShaderNodeAttribute") iferr_return;
	_graph.SetProperty(node, "attribute_type", "INSTANCER"_s);
	_graph.SetProperty(node, "attribute_name", "mograph_color"_s);
	return NodeOutput { node, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Invert(const NodeOutput& color)
{
	iferr_scope;

	const maxon::Int invert = _graph.Add("ShaderNodeInvert") iferr_return;
	_graph.SetInput(invert, "Fac", 1.0);
	_graph.Link(color, invert, "Color") iferr_return;
	return NodeOutput { invert, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Brightness(const NodeOutput& color)
{
	iferr_scope;

	// Cinema 4D's shaders weigh the three channels equally when they need one number.
	const maxon::Int separate = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.Link(color, separate, "Color") iferr_return;
	const maxon::Int sum = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(sum, "operation", "ADD"_s);
	_graph.Link(NodeOutput { separate, "Red"_s }, sum, "Value") iferr_return;
	_graph.Link(NodeOutput { separate, "Green"_s }, sum, "Value_001") iferr_return;
	const maxon::Int mean = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(mean, "operation", "MULTIPLY_ADD"_s);
	_graph.Link(NodeOutput { separate, "Blue"_s }, mean, "Value") iferr_return;
	_graph.SetInput(mean, "Value_001", 1.0 / 3.0);
	const maxon::Int scaled = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(scaled, "operation", "MULTIPLY"_s);
	_graph.Link(NodeOutput { sum, "Value"_s }, scaled, "Value") iferr_return;
	_graph.SetInput(scaled, "Value_001", 1.0 / 3.0);
	_graph.Link(NodeOutput { scaled, "Value"_s }, mean, "Value_002") iferr_return;
	return NodeOutput { mean, "Value"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Coordinate(maxon::Bool v)
{
	iferr_scope;

	NodeOutput uv = _uv;
	if (!uv.IsValid())
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}
	const maxon::Int separate = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
	_graph.Link(uv, separate, "Vector") iferr_return;
	// Measured against the Standard renderer: the gradient along V starts at the bottom of the
	// UV square, which is where Blender's V starts too.
	return NodeOutput { separate, v ? "Y"_s : "X"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Ramp(cinema::BaseShader* shader, maxon::Int32 id, const NodeOutput& factor)
{
	iferr_scope;

	const cinema::GeData data = GetParameterData(shader, id);
	const cinema::Gradient* const gradient = static_cast<const cinema::Gradient*>(data.GetCustomDataTypeI(CUSTOMDATATYPE_GRADIENT));
	if (!gradient)
		return NodeOutput();
	const maxon::Int32 count = gradient->GetKnotCount();
	if (count < 1 || count > MAX_RAMP_STOPS)
		return NodeOutput();

	// A Color Ramp has one interpolation for all its stops and no bias between them.
	maxon::BaseArray<maxon::GradientKnot> knots;
	for (maxon::Int32 i = 0; i < count; ++i)
	{
		const maxon::GradientKnot knot = gradient->GetKnot(i);
		if (knot.interpolation != gradient->GetKnot(0).interpolation || (count > 1 && maxon::Abs(knot.bias - 0.5) > 1e-4))
			return NodeOutput();
		knots.Append(knot) iferr_return;
	}
	const maxon::Char* interpolation = nullptr;
	switch (knots[0].interpolation)
	{
		case maxon::GRADIENT_INTERPOLATION_TYPE::LINEARKNOT: interpolation = "LINEAR"; break;
		case maxon::GRADIENT_INTERPOLATION_TYPE::SMOOTHKNOT: interpolation = "EASE"; break;
		case maxon::GRADIENT_INTERPOLATION_TYPE::NONE: interpolation = "CONSTANT"; break;
		default: return NodeOutput();
	}
	// By position; knots at one position keep their order.
	for (maxon::Int i = 1; i < knots.GetCount(); ++i)
	{
		for (maxon::Int k = i; k > 0 && knots[k].pos < knots[k - 1].pos; --k)
			std::swap(knots[k], knots[k - 1]);
	}

	JsonWriter ramp;
	ramp.BeginObject();
	ramp.String("interpolation", maxon::String(interpolation));
	ramp.BeginArray("elements");
	for (const maxon::GradientKnot& knot : knots)
	{
		// A knot is a colour parameter like any other (measured with a stepped gradient in "Basic" mode).
		const cinema::Vector color = _exporter.GetColors().Input(cinema::Vector(knot.col.r, knot.col.g, knot.col.b) * knot.brightness);
		const maxon::Float64 element[5] = { knot.pos, color.x, color.y, color.z, 1.0 };
		ramp.Floats(nullptr, element, 5);
	}
	ramp.EndArray();
	ramp.EndObject();

	const maxon::Int node = _graph.Add("ShaderNodeValToRGB") iferr_return;
	_graph.SetPropertyJson(node, "color_ramp", ramp.GetText());
	_graph.Link(factor, node, "Fac") iferr_return;
	return NodeOutput { node, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::GradientShader(cinema::BaseShader* shader)
{
	iferr_scope;

	// Along U or V a gradient is a Color Ramp over one coordinate. The other types and
	// turbulence are baked. ("Cycle" makes no difference between 0 and 1.)
	const maxon::Int32 type = GetIntParameter(shader, SLA_GRADIENT_TYPE, SLA_GRADIENT_TYPE_2D_U);
	if ((type != SLA_GRADIENT_TYPE_2D_U && type != SLA_GRADIENT_TYPE_2D_V) ||
		GetFloatParameter(shader, SLA_GRADIENT_TURBULENCE, 0.0) != 0.0 || GetFloatParameter(shader, SLA_GRADIENT_ANGLE, 0.0) != 0.0)
		return NodeOutput();
	const NodeOutput coordinate = Coordinate(type == SLA_GRADIENT_TYPE_2D_V) iferr_return;
	return Ramp(shader, SLA_GRADIENT_GRADIENT, coordinate);
}

maxon::Result<NodeOutput> ShaderTranslator::Colorizer(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	cinema::BaseShader* const inputShader = GetShaderParameter(shader, SLA_COLORIZER_TEXTURE, _exporter.GetDocument());
	const maxon::Int32 input = GetIntParameter(shader, SLA_COLORIZER_INPUT, SLA_COLORIZER_INPUT_LUMINANCE);
	if (!inputShader || (input != SLA_COLORIZER_INPUT_LUMINANCE && input != SLA_COLORIZER_INPUT_RED && input != SLA_COLORIZER_INPUT_GREEN && input != SLA_COLORIZER_INPUT_BLUE))
		return NodeOutput();
	// Baking the whole colorizer is as good as baking its input, and one texture less to look up.
	if (CanBake(inputShader))
	{
		const NodeOutput inputNative = Native(inputShader, colorData) iferr_return;
		if (!inputNative.IsValid())
			return NodeOutput();
	}

	const NodeOutput color = Any(inputShader, colorData) iferr_return;
	if (!color.IsValid())
		return NodeOutput();
	NodeOutput factor;
	if (input == SLA_COLORIZER_INPUT_LUMINANCE)
	{
		factor = Brightness(color) iferr_return;
	}
	else
	{
		const maxon::Int separate = _graph.Add("ShaderNodeSeparateColor") iferr_return;
		_graph.Link(color, separate, "Color") iferr_return;
		factor = NodeOutput { separate, input == SLA_COLORIZER_INPUT_RED ? "Red"_s : (input == SLA_COLORIZER_INPUT_GREEN ? "Green"_s : "Blue"_s) };
	}
	return Ramp(shader, SLA_COLORIZER_GRADIENT, factor);
}

maxon::Result<NodeOutput> ShaderTranslator::Fresnel(cinema::BaseShader* shader)
{
	iferr_scope;

	// The gradient runs over the angle between the surface and the view: its left end where
	// the surface is seen edge-on, its right end where it faces the viewer.
	if (GetBoolParameter(shader, SLA_FRESNEL_PHY_ENABLE, false))
		return NodeOutput();
	const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
	const maxon::Int dot = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(dot, "operation", "DOT_PRODUCT"_s);
	_graph.Link(NodeOutput { geometry, "Normal"_s }, dot, "Vector") iferr_return;
	_graph.Link(NodeOutput { geometry, "Incoming"_s }, dot, "Vector_001") iferr_return;
	const maxon::Int facing = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(facing, "operation", "ABSOLUTE"_s);
	_graph.Link(NodeOutput { dot, "Value"_s }, facing, "Value") iferr_return;
	return Ramp(shader, SLA_FRESNEL_GRADIENT, NodeOutput { facing, "Value"_s });
}

maxon::Result<NodeOutput> ShaderTranslator::Blend(maxon::Int32 layerBlendMode, const NodeOutput& bottom, const NodeOutput& top, maxon::Float64 amount, const NodeOutput& mask)
{
	iferr_scope;

	// Only the modes whose formula is the same in Blender's Mix node.
	const maxon::Char* type = nullptr;
	switch (layerBlendMode)
	{
		case cinema::BlendNormal: type = "MIX"; break;
		case cinema::BlendMultiply: type = "MULTIPLY"; break;
		case cinema::BlendScreen: type = "SCREEN"; break;
		case cinema::BlendDarken: type = "DARKEN"; break;
		case cinema::BlendLighten: type = "LIGHTEN"; break;
		case cinema::BlendAdd: type = "ADD"; break;
		case cinema::BlendSubtract: type = "SUBTRACT"; break;
		case cinema::BlendDifference: type = "DIFFERENCE"; break;
		case cinema::BlendExclusion: type = "EXCLUSION"; break;
		default: return NodeOutput();
	}
	const maxon::Int mix = _graph.Add("ShaderNodeMix") iferr_return;
	_graph.SetProperty(mix, "data_type", "RGBA"_s);
	_graph.SetProperty(mix, "blend_type", maxon::String(type));
	_graph.SetProperty(mix, "clamp_factor", true);
	if (bottom.IsValid())
	{
		_graph.Link(bottom, mix, "A_Color") iferr_return;
	}
	else
	{
		_graph.SetInput(mix, "A_Color", cinema::Vector(0.0));
	}
	_graph.Link(top, mix, "B_Color") iferr_return;
	if (mask.IsValid())
	{
		const maxon::Int scale = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(scale, "operation", "MULTIPLY"_s);
		_graph.Link(mask, scale, "Value") iferr_return;
		_graph.SetInput(scale, "Value_001", amount);
		_graph.Link(NodeOutput { scale, "Value"_s }, mix, "Factor_Float") iferr_return;
	}
	else
	{
		_graph.SetInput(mix, "Factor_Float", amount);
	}
	return NodeOutput { mix, "Result_Color"_s };
}

// The Fusion shader's blend modes, in the Layer shader's numbering.
static maxon::Int32 FusionBlendMode(maxon::Int32 mode)
{
	switch (mode)
	{
		case SLA_FUSION_MODE_NORMAL: return cinema::BlendNormal;
		case SLA_FUSION_MODE_MULTIPLY: return cinema::BlendMultiply;
		case SLA_FUSION_MODE_SCREEN: return cinema::BlendScreen;
		case SLA_FUSION_MODE_DARKEN: return cinema::BlendDarken;
		case SLA_FUSION_MODE_LIGHTEN: return cinema::BlendLighten;
		case SLA_FUSION_MODE_ADD: return cinema::BlendAdd;
		case SLA_FUSION_MODE_SUBTRACT: return cinema::BlendSubtract;
		case SLA_FUSION_MODE_DIFFERENCE: return cinema::BlendDifference;
		case SLA_FUSION_MODE_EXCLUSION: return cinema::BlendExclusion;
		default: return NOTOK;
	}
}

maxon::Result<NodeOutput> ShaderTranslator::Fusion(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	cinema::BaseDocument* const doc = _exporter.GetDocument();
	cinema::BaseShader* const baseShader = GetShaderParameter(shader, SLA_FUSION_BASE_CHANNEL, doc);
	cinema::BaseShader* const blendShader = GetShaderParameter(shader, SLA_FUSION_BLEND_CHANNEL, doc);
	cinema::BaseShader* const maskShader = GetBoolParameter(shader, SLA_FUSION_USE_MASK, false) ? GetShaderParameter(shader, SLA_FUSION_MASK_CHANNEL, doc) : nullptr;
	const maxon::Int32 mode = FusionBlendMode(GetIntParameter(shader, SLA_FUSION_MODE, SLA_FUSION_MODE_NORMAL));
	if (mode == NOTOK || !blendShader)
		return NodeOutput();
	// When everything can be baked, one baked texture beats a graph of several.
	if (CanBake(shader))
	{
		for (cinema::BaseShader* part : { baseShader, blendShader, maskShader })
		{
			if (!part)
				continue;
			const maxon::Bool simple = part->GetType() == Xbitmap || part->GetType() == Xgradient || part->GetType() == Xcolor || part->GetType() == Xcheckerboard;
			if (!simple)
				return NodeOutput();
		}
	}

	const NodeOutput base = Any(baseShader, colorData) iferr_return;
	const NodeOutput blend = Any(blendShader, colorData) iferr_return;
	if (!blend.IsValid() || (baseShader && !base.IsValid()))
		return NodeOutput();
	NodeOutput mask;
	if (maskShader)
	{
		const NodeOutput maskColor = Any(maskShader, false) iferr_return;
		if (!maskColor.IsValid())
			return NodeOutput();
		mask = Brightness(maskColor) iferr_return;
		if (GetBoolParameter(shader, SLA_FUSION_INVERT_MASK, false))
		{
			const maxon::Int invert = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(invert, "operation", "SUBTRACT"_s);
			_graph.SetInput(invert, "Value", 1.0);
			_graph.Link(mask, invert, "Value_001") iferr_return;
			mask = NodeOutput { invert, "Value"_s };
		}
	}
	NodeOutput result = Blend(mode, base, blend, GetFloatParameter(shader, SLA_FUSION_BLEND, 1.0), mask) iferr_return;
	if (result.IsValid() && GetBoolParameter(shader, SLA_FUSION_INVERT_OUTPUT, false))
	{
		result = Invert(result) iferr_return;
	}
	return result;
}

maxon::Result<NodeOutput> ShaderTranslator::Layer(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	// A graph is only worth building when a layer cannot be baked; otherwise the whole stack
	// becomes one texture, which also covers the effect layers and every blend mode.
	if (CanBake(shader))
		return NodeOutput();

	cinema::LayerShader* const layers = static_cast<cinema::LayerShader*>(shader);
	// The list starts with the top layer; the stack is built from the bottom.
	maxon::BaseArray<cinema::LayerShaderLayer*> stack;
	for (cinema::LayerShaderLayer* layer = layers->GetFirstLayer(); layer; layer = layer->GetNext())
	{
		cinema::GeData active;
		if (layer->GetParameter(LAYER_S_PARAM_ALL_ACTIVE, active) && !active.GetBool())
			continue;
		cinema::GeData flags;
		if (layer->GetType() != cinema::TypeShader || (layer->GetParameter(LAYER_S_PARAM_ALL_FLAGS, flags) && flags.GetInt32() != 0))
			return NodeOutput();
		stack.Insert(0, layer) iferr_return;
	}

	NodeOutput result;
	for (cinema::LayerShaderLayer* layer : stack)
	{
		cinema::GeData mode;
		cinema::GeData amount;
		cinema::GeData link;
		layer->GetParameter(LAYER_S_PARAM_SHADER_MODE, mode);
		layer->GetParameter(LAYER_S_PARAM_SHADER_BLEND, amount);
		layer->GetParameter(LAYER_S_PARAM_SHADER_LINK, link);
		cinema::BaseLink* const shaderLink = static_cast<cinema::BaseLink*>(link.GetVoid());
		cinema::BaseList2D* const linked = shaderLink ? shaderLink->GetLink(_exporter.GetDocument(), Xbase) : nullptr;
		if (!linked)
			continue;
		const NodeOutput color = Any(static_cast<cinema::BaseShader*>(linked), colorData) iferr_return;
		if (!color.IsValid())
			return NodeOutput();
		result = Blend(mode.GetInt32(), result, color, amount.GetFloat(), NodeOutput()) iferr_return;
		if (!result.IsValid())
			return NodeOutput();
	}
	return result;
}

} // namespace cyclesrender
