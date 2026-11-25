// SPDX-License-Identifier: Apache-2.0
// The Layer shader as Blender nodes (ShaderTranslator::Layer, shader_translate.h): its layers from the bottom up,
// each shader layer blended onto what is below it (shader_blend.cpp), each effect layer applied to it.
//
// Measured with the Standard renderer of Cinema 4D 2027.0.0 (internal/f13/probe_w32_layers.py: a base gradient
// with one effect layer on top, render-space values of an OCIO document, the antialiased border left out):
//   Brightness/Contrast  brightness b: a + b, held at 0 below but not at 1 above; contrast c: (a - 0.5)(1 + c) + 0.5,
//                        held at 0 below; gamma g: a^(1/g) (each alone, exact)
//   HSL                  hue h degrees: HSV's hue turned by h/360; saturation s: HSV's saturation plus s, held to
//                        0..1; lightness l: towards white by l (a + (1 - a) l) or towards black by -l (a (1 + l))
//                        (each alone, within 0.009)
//   Posterize            levels n: each channel to the nearest of 0, 1/n, ..., 1 (n = 4, the width as made)
//   Clamp                low, high: a held to low..high
//   Clip                 low, high: (a - low) / (high - low) held to 0..1
//   Alpha Mask (a blend mode)  changes no colour below it: it is the stack's alpha, which no colour channel shows
// Several settings of one Brightness/Contrast or HSL layer at once were not measured: they are applied in the
// order above, which is assumed. Not measured, so left out and named: Colorize and HSL's own Colorize, Distorter,
// Transform, folders (Python cannot put a layer into a folder, so none could be measured), a layer's invert and
// alpha flags.

#include "c4d.h"
#include "lib_layershader.h"

#include "material_export.h"
#include "shader_translate.h"

namespace cyclesrender
{

void ShaderTranslator::NoteLayer(const maxon::String& what)
{
	_exporter.GetWriter().Warn(FormatString("Material \"@\": in the Layer shader of the @ channel, @; that layer is left out.", _materialName, _channel, what));
}

static maxon::Float64 LayerFloat(cinema::LayerShaderLayer* layer, maxon::Int32 id, maxon::Float64 fallback)
{
	cinema::GeData data;
	return layer->GetParameter(id, data) ? data.GetFloat() : fallback;
}

static maxon::Int32 LayerInt(cinema::LayerShaderLayer* layer, maxon::Int32 id, maxon::Int32 fallback)
{
	cinema::GeData data;
	return layer->GetParameter(id, data) ? data.GetInt32() : fallback;
}

// A Vector Math node of @p operation on @p a and a constant vector (or a scale).
static maxon::Result<NodeOutput> VectorWith(NodeGraph& graph, const maxon::Char* operation, const NodeOutput& a, const cinema::Vector& b,
	const cinema::Vector& c = cinema::Vector(0.0))
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeVectorMath") iferr_return;
	graph.SetProperty(node, "operation", maxon::String(operation));
	graph.Link(a, node, "Vector") iferr_return;
	graph.SetInput(node, "Vector_001", b);
	graph.SetInput(node, "Vector_002", c);
	return NodeOutput { node, "Vector"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::EffectLayer(cinema::LayerShaderLayer* layer, const NodeOutput& below, maxon::String& what)
{
	iferr_scope;
	NodeOutput color = below;
	if (!color.IsValid())
	{
		color = Black() iferr_return;
	}
	switch (layer->GetType())
	{
		case cinema::TypeBrightnessContrast:
		{
			const maxon::Float64 brightness = LayerFloat(layer, LAYER_S_PARAM_BC_BRIGHTNESS, 0.0);
			const maxon::Float64 contrast = LayerFloat(layer, LAYER_S_PARAM_BC_CONTRAST, 0.0);
			const maxon::Float64 gamma = LayerFloat(layer, LAYER_S_PARAM_BC_GAMMA, 1.0);
			if (brightness != 0.0)
			{
				color = VectorWith(_graph, "ADD", color, cinema::Vector(brightness)) iferr_return;
				color = VectorWith(_graph, "MAXIMUM", color, cinema::Vector(0.0)) iferr_return;
			}
			if (contrast != 0.0)
			{
				color = VectorWith(_graph, "MULTIPLY_ADD", color, cinema::Vector(1.0 + contrast), cinema::Vector(0.5 - 0.5 * (1.0 + contrast))) iferr_return;
				color = VectorWith(_graph, "MAXIMUM", color, cinema::Vector(0.0)) iferr_return;
			}
			if (gamma != 1.0 && gamma > 0.0)
			{
				const maxon::Int power = _graph.Add("ShaderNodeGamma") iferr_return;
				_graph.Link(color, power, "Color") iferr_return;
				_graph.SetInput(power, "Gamma", 1.0 / gamma);
				color = NodeOutput { power, "Color"_s };
			}
			return color;
		}
		case cinema::TypeHSL:
		{
			if (LayerInt(layer, LAYER_S_PARAM_HSL_COLORIZE, 0) != 0)
			{
				what = "an HSL layer's Colorize is not translated"_s;
				return NodeOutput();
			}
			const maxon::Float64 hue = LayerFloat(layer, LAYER_S_PARAM_HSL_HUE, 0.0);
			const maxon::Float64 saturation = LayerFloat(layer, LAYER_S_PARAM_HSL_SATURATION, 0.0);
			const maxon::Float64 lightness = LayerFloat(layer, LAYER_S_PARAM_HSL_LIGHTNESS, 0.0);
			if (hue != 0.0 || saturation != 0.0)
			{
				const maxon::Int separate = _graph.Add("ShaderNodeSeparateColor") iferr_return;
				_graph.SetProperty(separate, "mode", "HSV"_s);
				_graph.Link(color, separate, "Color") iferr_return;
				const maxon::Int turned = _graph.Add("ShaderNodeMath") iferr_return;
				_graph.SetProperty(turned, "operation", "WRAP"_s);
				const maxon::Int add = _graph.Add("ShaderNodeMath") iferr_return;
				_graph.SetProperty(add, "operation", "ADD"_s);
				_graph.Link(NodeOutput { separate, "Red"_s }, add, "Value") iferr_return;
				_graph.SetInput(add, "Value_001", hue / 360.0);
				_graph.Link(NodeOutput { add, "Value"_s }, turned, "Value") iferr_return;
				_graph.SetInput(turned, "Value_001", 1.0);
				_graph.SetInput(turned, "Value_002", 0.0);
				const maxon::Int saturated = _graph.Add("ShaderNodeMath") iferr_return;
				_graph.SetProperty(saturated, "operation", "ADD"_s);
				_graph.SetProperty(saturated, "use_clamp", true);
				_graph.Link(NodeOutput { separate, "Green"_s }, saturated, "Value") iferr_return;
				_graph.SetInput(saturated, "Value_001", saturation);
				const maxon::Int combine = _graph.Add("ShaderNodeCombineColor") iferr_return;
				_graph.SetProperty(combine, "mode", "HSV"_s);
				_graph.Link(NodeOutput { turned, "Value"_s }, combine, "Red") iferr_return;
				_graph.Link(NodeOutput { saturated, "Value"_s }, combine, "Green") iferr_return;
				_graph.Link(NodeOutput { separate, "Blue"_s }, combine, "Blue") iferr_return;
				color = NodeOutput { combine, "Color"_s };
			}
			if (lightness != 0.0)
			{
				const maxon::Int towards = _graph.Add("ShaderNodeCombineColor") iferr_return;
				const maxon::Float64 end = lightness > 0.0 ? 1.0 : 0.0;
				_graph.SetInput(towards, "Red", end);
				_graph.SetInput(towards, "Green", end);
				_graph.SetInput(towards, "Blue", end);
				color = MixColors("MIX", color, NodeOutput { towards, "Color"_s }, NodeOutput(), maxon::Abs(lightness)) iferr_return;
			}
			return color;
		}
		case cinema::TypePosterize:
		{
			const maxon::Int32 levels = LayerInt(layer, LAYER_S_PARAM_POSTER_LEVELS, 0);
			if (levels < 1 || maxon::Abs(LayerFloat(layer, LAYER_S_PARAM_POSTER_WIDTH, 1.0) - 1.0) > 1e-6)
			{
				what = "a Posterize layer's width other than 1 is not translated"_s;
				return NodeOutput();
			}
			// The nearest of 0, 1/n, ..., 1: snapped down after half a step up.
			const maxon::Float64 step = 1.0 / levels;
			const NodeOutput lifted = VectorWith(_graph, "ADD", color, cinema::Vector(0.5 * step)) iferr_return;
			return VectorWith(_graph, "SNAP", lifted, cinema::Vector(step));
		}
		case cinema::TypeClamp:
		{
			const maxon::Float64 low = LayerFloat(layer, LAYER_S_PARAM_CLAMP_LOW_CLIP, 0.0);
			const maxon::Float64 high = LayerFloat(layer, LAYER_S_PARAM_CLAMP_HIGH_CLIP, 1.0);
			color = VectorWith(_graph, "MAXIMUM", color, cinema::Vector(low)) iferr_return;
			return VectorWith(_graph, "MINIMUM", color, cinema::Vector(high));
		}
		case cinema::TypeClip:
		{
			const maxon::Float64 low = LayerFloat(layer, LAYER_S_PARAM_CLIP_LOW_CLIP, 0.0);
			const maxon::Float64 high = LayerFloat(layer, LAYER_S_PARAM_CLIP_HIGH_CLIP, 1.0);
			if (high <= low)
			{
				what = "a Clip layer whose high value is not above its low value is not translated"_s;
				return NodeOutput();
			}
			color = VectorWith(_graph, "MULTIPLY_ADD", color, cinema::Vector(1.0 / (high - low)), cinema::Vector(-low / (high - low))) iferr_return;
			color = VectorWith(_graph, "MAXIMUM", color, cinema::Vector(0.0)) iferr_return;
			return VectorWith(_graph, "MINIMUM", color, cinema::Vector(1.0));
		}
		case cinema::TypeColorize: what = "a Colorize layer is not translated"_s; break;
		case cinema::TypeDistorter: what = "a Distorter layer is not translated"_s; break;
		case cinema::TypeTransform: what = "a Transformation layer is not translated"_s; break;
		case cinema::TypeFolder: what = "a folder is not translated"_s; break;
		default: what = "a layer of an unknown kind is not translated"_s; break;
	}
	return NodeOutput();
}

maxon::Result<NodeOutput> ShaderTranslator::Layer(cinema::BaseShader* shader, maxon::Bool colorData)
{
	iferr_scope;

	// A graph is only worth building when a layer cannot be baked; otherwise the whole stack
	// becomes one texture, which also covers every layer type and blend mode exactly.
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
		stack.Insert(0, layer) iferr_return;
	}

	NodeOutput result;
	for (cinema::LayerShaderLayer* layer : stack)
	{
		const maxon::String name = layer->GetName(_exporter.GetDocument());
		// A layer's "Alpha" makes its shader's alpha its opacity over what is below; at the bottom of the stack
		// nothing is below, and the colour is the layer's whatever its alpha (the Toon scene's fur bitmaps).
		const maxon::Int32 flags = LayerInt(layer, LAYER_S_PARAM_ALL_FLAGS, 0);
		if (flags != 0 && !(flags == cinema::LAYER_ALPHA && !result.IsValid()))
		{
			NoteLayer(FormatString("the layer \"@\" has its invert or alpha switches on, which are not translated", name));
			continue;
		}
		if (layer->GetType() != cinema::TypeShader)
		{
			maxon::String what;
			const NodeOutput applied = EffectLayer(layer, result, what) iferr_return;
			if (applied.IsValid())
				result = applied;
			else
				NoteLayer(FormatString("@ (\"@\")", what, name));
			continue;
		}
		const maxon::Int32 mode = LayerInt(layer, LAYER_S_PARAM_SHADER_MODE, cinema::BlendNormal);
		// Measured: Alpha Mask changes no colour of the stack (it is its alpha, which no colour channel shows).
		if (mode == cinema::BlendAlphaMask)
			continue;
		cinema::GeData link;
		layer->GetParameter(LAYER_S_PARAM_SHADER_LINK, link);
		cinema::BaseLink* const shaderLink = static_cast<cinema::BaseLink*>(link.GetVoid());
		cinema::BaseList2D* const linked = shaderLink ? shaderLink->GetLink(_exporter.GetDocument(), Xbase) : nullptr;
		if (!linked)
			continue;
		cinema::BaseShader* const part = static_cast<cinema::BaseShader*>(linked);
		const NodeOutput color = Any(part, colorData) iferr_return;
		if (!color.IsValid())
		{
			NoteLayer(FormatString("the @ shader (\"@\") is not translated", part->GetTypeName(), name));
			continue;
		}
		const NodeOutput blended = Blend(mode, result, color, LayerFloat(layer, LAYER_S_PARAM_SHADER_BLEND, 1.0), NodeOutput()) iferr_return;
		if (!blended.IsValid())
		{
			NoteLayer(FormatString("the blend mode of the layer \"@\" (Levr below 100 %) is not translated", name));
			continue;
		}
		result = blended;
	}
	return result;
}

} // namespace cyclesrender
