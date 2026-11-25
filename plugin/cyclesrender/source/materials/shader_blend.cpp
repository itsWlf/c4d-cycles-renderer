// SPDX-License-Identifier: Apache-2.0
// The blend modes of the Fusion and Layer shaders as Blender nodes (ShaderTranslator::Blend, shader_translate.h).
//
// Measured with the Standard renderer of Cinema 4D 2027.0.0 (internal/f13/probe_w32_modes.py: a base gradient a
// along U and a blend gradient b along V mixed by each mode, the operands rendered alone too, grey and coloured, at
// amounts 1 and 0.5, through the Fusion shader and the Layer shader alike, render-space values of an OCIO document).
// Every mode is the base mixed with its result by the amount, Levr aside; at full strength, per channel:
//   Normal b; Multiply ab; Screen 1 - (1 - a)(1 - b); Overlay a < 0.5: 2ab, else 1 - 2(1 - a)(1 - b); Hard Light the
//   same with b deciding; Soft Light a < 0.5: b (a + 0.5), else 1 - (1 - b)(1.5 - a); Add a + b and Subtract a - b
//   held to 0..1; Difference |a - b|; Exclusion a + b - 2ab (all within 0.007 on grey and on colours, the antialiased
//   border left out). On the whole colour: Dodge a with its HSL lightness 1 - (1 - La)(1 - Lb), Burn with La (1 - Lb)
//   (0.006; on greys they look like Screen and a (1 - b)); Darken and Lighten the colour of the lower or higher mean
//   of its channels, a or b whole (exact; not the channel's minimum); Hue b's hue in HSV (0.2 where b is nearly
//   grey); Color b's hue and saturation in HSV (0.018); Saturation b's saturation in HSL (0.015); Luminance b's
//   lightness in HSL (0.013); Levr 1 where a > b, else 0, and at an amount of 0.5 a ramp that is no mix (not
//   translated then).

#include "c4d.h"
#include "lib_layershader.h"

#include "shader_translate.h"

namespace cyclesrender
{

maxon::Result<NodeOutput> ShaderTranslator::Black()
{
	iferr_scope;
	const maxon::Int node = _graph.Add("ShaderNodeCombineColor") iferr_return;
	_graph.SetInput(node, "Red", 0.0);
	_graph.SetInput(node, "Green", 0.0);
	_graph.SetInput(node, "Blue", 0.0);
	return NodeOutput { node, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::MixColors(const maxon::Char* blendType, const NodeOutput& a, const NodeOutput& b, const NodeOutput& factor,
	maxon::Float64 amount, maxon::Bool clampResult)
{
	iferr_scope;
	const maxon::Int mix = _graph.Add("ShaderNodeMix") iferr_return;
	_graph.SetProperty(mix, "data_type", "RGBA"_s);
	_graph.SetProperty(mix, "blend_type", maxon::String(blendType));
	_graph.SetProperty(mix, "clamp_factor", true);
	if (clampResult)
		_graph.SetProperty(mix, "clamp_result", true);
	if (a.IsValid())
	{
		_graph.Link(a, mix, "A_Color") iferr_return;
	}
	else
	{
		_graph.SetInput(mix, "A_Color", cinema::Vector(0.0));
	}
	_graph.Link(b, mix, "B_Color") iferr_return;
	if (factor.IsValid())
	{
		_graph.Link(factor, mix, "Factor_Float") iferr_return;
	}
	else
	{
		_graph.SetInput(mix, "Factor_Float", amount);
	}
	return NodeOutput { mix, "Result_Color"_s };
}

// A Vector Math node of @p operation on two outputs, or on an output and a constant (when @p b is invalid).
static maxon::Result<NodeOutput> VectorOf(NodeGraph& graph, const maxon::Char* operation, const NodeOutput& a, const NodeOutput& b,
	const cinema::Vector& constant = cinema::Vector(0.0), maxon::Bool constantFirst = false)
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeVectorMath") iferr_return;
	graph.SetProperty(node, "operation", maxon::String(operation));
	const maxon::Char* first = constantFirst ? "Vector_001" : "Vector";
	const maxon::Char* second = constantFirst ? "Vector" : "Vector_001";
	graph.Link(a, node, first) iferr_return;
	if (b.IsValid())
	{
		graph.Link(b, node, second) iferr_return;
	}
	else
	{
		graph.SetInput(node, second, constant);
	}
	return NodeOutput { node, "Vector"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::SoftLight(const NodeOutput& a, const NodeOutput& b)
{
	iferr_scope;
	// Dark: b (a + 0.5); light: 1 - (1 - b)(1.5 - a); chosen per channel by a >= 0.5.
	const NodeOutput lifted = VectorOf(_graph, "ADD", a, NodeOutput(), cinema::Vector(0.5)) iferr_return;
	const NodeOutput dark = VectorOf(_graph, "MULTIPLY", lifted, b) iferr_return;
	const NodeOutput restB = VectorOf(_graph, "SUBTRACT", b, NodeOutput(), cinema::Vector(1.0), true) iferr_return;
	const NodeOutput restA = VectorOf(_graph, "SUBTRACT", a, NodeOutput(), cinema::Vector(1.5), true) iferr_return;
	const NodeOutput product = VectorOf(_graph, "MULTIPLY", restB, restA) iferr_return;
	const NodeOutput light = VectorOf(_graph, "SUBTRACT", product, NodeOutput(), cinema::Vector(1.0), true) iferr_return;

	const maxon::Int separate = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
	_graph.Link(a, separate, "Vector") iferr_return;
	const maxon::Int combine = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	static const maxon::Char* const AXES[3] = { "X", "Y", "Z" };
	for (const maxon::Char* axis : AXES)
	{
		const maxon::Int step = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(step, "operation", "GREATER_THAN"_s);
		_graph.Link(NodeOutput { separate, maxon::String(axis) }, step, "Value") iferr_return;
		_graph.SetInput(step, "Value_001", 0.5 - 1e-6);
		_graph.Link(NodeOutput { step, "Value"_s }, combine, axis) iferr_return;
	}
	const maxon::Int choose = _graph.Add("ShaderNodeMix") iferr_return;
	_graph.SetProperty(choose, "data_type", "VECTOR"_s);
	_graph.SetProperty(choose, "factor_mode", "NON_UNIFORM"_s);
	_graph.Link(NodeOutput { combine, "Vector"_s }, choose, "Factor_Vector") iferr_return;
	_graph.Link(dark, choose, "A_Vector") iferr_return;
	_graph.Link(light, choose, "B_Vector") iferr_return;
	return NodeOutput { choose, "Result_Vector"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::HslMix(const NodeOutput& a, const NodeOutput& b, maxon::Bool lightness)
{
	iferr_scope;
	// HSL's hue, saturation and lightness are Separate Color's Red, Green and Blue in its HSL mode.
	const maxon::Int fromA = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.SetProperty(fromA, "mode", "HSL"_s);
	_graph.Link(a, fromA, "Color") iferr_return;
	const maxon::Int fromB = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.SetProperty(fromB, "mode", "HSL"_s);
	_graph.Link(b, fromB, "Color") iferr_return;
	const maxon::Int combine = _graph.Add("ShaderNodeCombineColor") iferr_return;
	_graph.SetProperty(combine, "mode", "HSL"_s);
	_graph.Link(NodeOutput { fromA, "Red"_s }, combine, "Red") iferr_return;
	_graph.Link(NodeOutput { lightness ? fromA : fromB, "Green"_s }, combine, "Green") iferr_return;
	_graph.Link(NodeOutput { lightness ? fromB : fromA, "Blue"_s }, combine, "Blue") iferr_return;
	return NodeOutput { combine, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::HslLightness(const NodeOutput& a, const NodeOutput& b, maxon::Bool dodge)
{
	iferr_scope;
	const maxon::Int fromA = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.SetProperty(fromA, "mode", "HSL"_s);
	_graph.Link(a, fromA, "Color") iferr_return;
	const maxon::Int fromB = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.SetProperty(fromB, "mode", "HSL"_s);
	_graph.Link(b, fromB, "Color") iferr_return;
	// 1 - Lb, then Burn: La (1 - Lb); Dodge: 1 - (1 - La)(1 - Lb).
	const maxon::Int restB = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(restB, "operation", "SUBTRACT"_s);
	_graph.SetInput(restB, "Value", 1.0);
	_graph.Link(NodeOutput { fromB, "Blue"_s }, restB, "Value_001") iferr_return;
	NodeOutput lightness;
	if (dodge)
	{
		const maxon::Int restA = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(restA, "operation", "SUBTRACT"_s);
		_graph.SetInput(restA, "Value", 1.0);
		_graph.Link(NodeOutput { fromA, "Blue"_s }, restA, "Value_001") iferr_return;
		const maxon::Int product = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(product, "operation", "MULTIPLY"_s);
		_graph.Link(NodeOutput { restA, "Value"_s }, product, "Value") iferr_return;
		_graph.Link(NodeOutput { restB, "Value"_s }, product, "Value_001") iferr_return;
		const maxon::Int flip = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(flip, "operation", "SUBTRACT"_s);
		_graph.SetInput(flip, "Value", 1.0);
		_graph.Link(NodeOutput { product, "Value"_s }, flip, "Value_001") iferr_return;
		lightness = NodeOutput { flip, "Value"_s };
	}
	else
	{
		const maxon::Int product = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(product, "operation", "MULTIPLY"_s);
		_graph.Link(NodeOutput { fromA, "Blue"_s }, product, "Value") iferr_return;
		_graph.Link(NodeOutput { restB, "Value"_s }, product, "Value_001") iferr_return;
		lightness = NodeOutput { product, "Value"_s };
	}
	const maxon::Int combine = _graph.Add("ShaderNodeCombineColor") iferr_return;
	_graph.SetProperty(combine, "mode", "HSL"_s);
	_graph.Link(NodeOutput { fromA, "Red"_s }, combine, "Red") iferr_return;
	_graph.Link(NodeOutput { fromA, "Green"_s }, combine, "Green") iferr_return;
	_graph.Link(lightness, combine, "Blue") iferr_return;
	return NodeOutput { combine, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::ByMean(const NodeOutput& a, const NodeOutput& b, maxon::Bool darker)
{
	iferr_scope;
	// a where its mean is the lower (Darken) or the higher (Lighten) one, b otherwise; ties keep a.
	const NodeOutput meanA = Brightness(a) iferr_return;
	const NodeOutput meanB = Brightness(b) iferr_return;
	const maxon::Int compare = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(compare, "operation", darker ? "GREATER_THAN"_s : "LESS_THAN"_s);
	_graph.Link(meanA, compare, "Value") iferr_return;
	_graph.Link(meanB, compare, "Value_001") iferr_return;
	return MixColors("MIX", a, b, NodeOutput { compare, "Value"_s }, 0.0);
}

maxon::Result<NodeOutput> ShaderTranslator::Levr(const NodeOutput& a, const NodeOutput& b)
{
	iferr_scope;
	const maxon::Int fromA = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.Link(a, fromA, "Color") iferr_return;
	const maxon::Int fromB = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.Link(b, fromB, "Color") iferr_return;
	const maxon::Int combine = _graph.Add("ShaderNodeCombineColor") iferr_return;
	static const maxon::Char* const CHANNELS[3] = { "Red", "Green", "Blue" };
	for (const maxon::Char* channel : CHANNELS)
	{
		const maxon::Int step = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(step, "operation", "GREATER_THAN"_s);
		_graph.Link(NodeOutput { fromA, maxon::String(channel) }, step, "Value") iferr_return;
		_graph.Link(NodeOutput { fromB, maxon::String(channel) }, step, "Value_001") iferr_return;
		_graph.Link(NodeOutput { step, "Value"_s }, combine, channel) iferr_return;
	}
	return NodeOutput { combine, "Color"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::Blend(maxon::Int32 layerBlendMode, const NodeOutput& bottom, const NodeOutput& top, maxon::Float64 amount,
	const NodeOutput& mask)
{
	iferr_scope;

	// The amount times the mask, or the amount alone.
	NodeOutput factor;
	if (mask.IsValid())
	{
		const maxon::Int scale = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(scale, "operation", "MULTIPLY"_s);
		_graph.Link(mask, scale, "Value") iferr_return;
		_graph.SetInput(scale, "Value_001", amount);
		factor = NodeOutput { scale, "Value"_s };
	}

	// Modes whose Mix node gives the same mix by the factor as the Standard renderer.
	const maxon::Char* same = nullptr;
	switch (layerBlendMode)
	{
		case cinema::BlendNormal: same = "MIX"; break;
		case cinema::BlendMultiply: same = "MULTIPLY"; break;
		case cinema::BlendScreen: same = "SCREEN"; break;
		case cinema::BlendOverlay: same = "OVERLAY"; break;
		case cinema::BlendDifference: same = "DIFFERENCE"; break;
		case cinema::BlendExclusion: same = "EXCLUSION"; break;
		case cinema::BlendHue: same = "HUE"; break;
		case cinema::BlendColor: same = "COLOR"; break;
		default: break;
	}
	if (same)
		return MixColors(same, bottom, top, factor, amount);

	// The others: the mode's result at full strength, then mixed with the base.
	NodeOutput base = bottom;
	if (!base.IsValid())
	{
		base = Black() iferr_return;
	}
	NodeOutput full;
	switch (layerBlendMode)
	{
		case cinema::BlendAdd:
		{
			full = MixColors("ADD", base, top, NodeOutput(), 1.0, true) iferr_return;
			break;
		}
		case cinema::BlendSubtract:
		{
			full = MixColors("SUBTRACT", base, top, NodeOutput(), 1.0, true) iferr_return;
			break;
		}
		case cinema::BlendHardLight:
		{
			full = MixColors("OVERLAY", top, base, NodeOutput(), 1.0) iferr_return;
			break;
		}
		case cinema::BlendDodge:
		case cinema::BlendBurn:
		{
			full = HslLightness(base, top, layerBlendMode == cinema::BlendDodge) iferr_return;
			break;
		}
		case cinema::BlendDarken:
		case cinema::BlendLighten:
		{
			full = ByMean(base, top, layerBlendMode == cinema::BlendDarken) iferr_return;
			break;
		}
		case cinema::BlendSoftLight:
		{
			full = SoftLight(base, top) iferr_return;
			break;
		}
		case cinema::BlendSat:
		case cinema::BlendLuminance:
		{
			full = HslMix(base, top, layerBlendMode == cinema::BlendLuminance) iferr_return;
			break;
		}
		case cinema::BlendLevr:
		{
			if (amount < 0.999)
				return NodeOutput();
			full = Levr(base, top) iferr_return;
			break;
		}
		default:
			return NodeOutput();
	}
	return MixColors("MIX", base, full, factor, amount);
}

} // namespace cyclesrender
