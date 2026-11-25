// SPDX-License-Identifier: Apache-2.0
// The shaders that read the lighting and the surroundings, as Blender nodes (shader_translate.h): Cel, Lumas
// (its diffuse part) and Ambient Occlusion; and the light list they read (MaterialExporter::GetShadingLights).
//
// Measured with the Standard renderer of Cinema 4D 2027.0.0 (internal/f13/probe_w33.py: a ball under a parallel
// camera, luminance only, one or two infinite lights):
//   Cel    the Diffuse gradient at max over the lights of max(0, N . L), whatever their brightness (a light of 0.5
//          gave the same as one of 1) and colour, within 0.026; two lights: the larger of the two (0.027; their
//          sum is off by 0.21); no light: the gradient's left end. Shadows only with its "Shadows" on (a box between
//          the light and the ball: no trace of it with the switch off, the default); then, with the shadow type
//          "Lights" and a black Shadow gradient, the shadow is the gradient's left end; the types Color and
//          Multiply colour it otherwise (not followed). Camera on: about the view direction (0.15; not followed).
//   Lumas  diffuse, algorithm Internal, contrast 0: colour x illumination x B N . L (0.8 x 0.6 x 0.98 = 0.47 at
//          0.98; half the brightness, half the value); contrast 1 is a step at 0.5 (the Toon scene's Lumas).
// The maintainer's choice (2026-11-25): the Cel shader exact in Eevee through Shader to RGB, a look-alike in
// Cycles, which has no Shader to RGB. Measured since: without its "Shadows" the Cel shader is exactly the largest
// light direction, which both engines then compute from the lights' directions (Shader to RGB would add the shadows
// the Cel shader leaves out); with "Shadows" on Eevee goes through Shader to RGB (the shadows exact for one light)
// and Cycles keeps the directions, without the shadows.

#include "c4d.h"
#include "olight.h"
#include "xambientocclusion.h"
#include "xcelshader.h"
#include "xslalumas.h"

#include "../scene/coords.h"
#include "../scene/light_export.h"
#include "../scene/light_falloff.h"
#include "../scene/parameters.h"
#include "material_export.h"
#include "shader_translate.h"

namespace cyclesrender
{

// More lights than this are not followed by the Cycles look-alike (a node chain per light).
static const maxon::Int MAX_SHADING_LIGHTS = 16;

static maxon::Result<void> CollectShadingLights(cinema::BaseObject* op, const Coords& coords, const DocumentColors& colors,
	maxon::BaseArray<ShadingLight>& lights)
{
	iferr_scope;
	for (; op; op = op->GetNext())
	{
		if (op->GetType() == Olight && IsLightSwitchedOn(op) && op->GetRenderMode() != MODE_OFF && !GetBoolParameter(op, LIGHT_NOLIGHTRADIATION, false) &&
			!GetBoolParameter(op, LIGHT_DETAILS_AMBIENT, false))
		{
			ShadingLight light;
			maxon::Bool photometric = false;
			light.brightness = GetFloatParameter(op, LIGHT_BRIGHTNESS, 1.0) * PhotometricFactor(op, photometric, nullptr);
			light.color = colors.Input(GetVectorParameter(op, LIGHT_COLOR, cinema::Vector(1.0)));
			const cinema::Matrix mg = op->GetMg();
			const maxon::Int32 type = GetIntParameter(op, LIGHT_TYPE, LIGHT_TYPE_OMNI);
			light.infinite = type == LIGHT_TYPE_DISTANT || type == LIGHT_TYPE_PARALLEL || type == LIGHT_TYPE_PARSPOT || type == LIGHT_TYPE_PARSPOTRECT;
			// A light shines along its +Z; Cinema 4D's x, y, z are Blender's x, z, y.
			const cinema::Vector towards = -mg.sqmat.v3.GetNormalized();
			light.direction = cinema::Vector(towards.x, towards.z, towards.y);
			maxon::Float32 place[3];
			coords.Point(mg.off, place);
			light.position = cinema::Vector(place[0], place[1], place[2]);
			lights.Append(light) iferr_return;
		}
		CollectShadingLights(op->GetDown(), coords, colors, lights) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<const maxon::BaseArray<ShadingLight>*> MaterialExporter::GetShadingLights()
{
	iferr_scope;
	if (!_shadingLightsRead && _doc)
	{
		_shadingLightsRead = true;
		CollectShadingLights(_doc->GetFirstObject(), Coords::FromDocument(_doc), _colors, _shadingLights) iferr_return;
	}
	return &_shadingLights;
}

// A Math node of @p operation on two outputs.
static maxon::Result<NodeOutput> MathOf2(NodeGraph& graph, const maxon::Char* operation, const NodeOutput& a, const NodeOutput& b)
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(node, "operation", maxon::String(operation));
	graph.Link(a, node, "Value") iferr_return;
	graph.Link(b, node, "Value_001") iferr_return;
	return NodeOutput { node, "Value"_s };
}

// A Value node holding @p value.
static maxon::Result<NodeOutput> Constant(NodeGraph& graph, maxon::Float64 value)
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeValue") iferr_return;
	graph.SetOutputJson(node, "Value", FormatString("@", value));
	return NodeOutput { node, "Value"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::LightTerm(maxon::Bool cel, maxon::Bool shadows)
{
	iferr_scope;
	const maxon::BaseArray<ShadingLight>* const lights = _exporter.GetShadingLights() iferr_return;

	if (_exporter.IsEevee() && shadows)
	{
		// The lighting of a white diffuse surface, as Eevee renders it with the lights' shadows: with the plugin's
		// light conversion a light of brightness B gives B N . L there, as in Cinema 4D.
		const maxon::Int diffuse = _graph.Add("ShaderNodeBsdfDiffuse") iferr_return;
		_graph.SetInput(diffuse, "Color", cinema::Vector(1.0));
		_graph.SetInput(diffuse, "Roughness", 0.0);
		const maxon::Int convert = _graph.Add("ShaderNodeShaderToRGB") iferr_return;
		_graph.Link(NodeOutput { diffuse, "BSDF"_s }, convert, "Shader") iferr_return;
		const NodeOutput color { convert, "Color"_s };
		if (!cel)
			return color;
		// The Cel shader counts a light's direction, not its brightness: the brightest channel divided by the
		// brightest light's (exact for one light; several add up here where Cinema 4D takes the largest).
		maxon::Float64 reference = 0.0;
		for (const ShadingLight& light : *lights)
			reference = maxon::Max(reference, light.brightness * maxon::Max(light.color.x, maxon::Max(light.color.y, light.color.z)));
		if (reference <= 0.0)
			return Constant(_graph, 0.0);
		const maxon::Int separate = _graph.Add("ShaderNodeSeparateColor") iferr_return;
		_graph.Link(color, separate, "Color") iferr_return;
		const NodeOutput redGreen = MathOf2(_graph, "MAXIMUM", NodeOutput { separate, "Red"_s }, NodeOutput { separate, "Green"_s }) iferr_return;
		const NodeOutput brightest = MathOf2(_graph, "MAXIMUM", redGreen, NodeOutput { separate, "Blue"_s }) iferr_return;
		const maxon::Int scale = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(scale, "operation", "MULTIPLY"_s);
		_graph.SetProperty(scale, "use_clamp", true);
		_graph.Link(brightest, scale, "Value") iferr_return;
		_graph.SetInput(scale, "Value_001", 1.0 / reference);
		return NodeOutput { scale, "Value"_s };
	}

	// Cycles: max(0, N . L) per light, from an infinite light's direction or towards a light's place.
	const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
	const NodeOutput normal { geometry, "Normal"_s };
	NodeOutput total;
	maxon::Int count = 0;
	for (const ShadingLight& light : *lights)
	{
		if (++count > MAX_SHADING_LIGHTS)
			break;
		NodeOutput towards;
		if (light.infinite)
		{
			const maxon::Int dot = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(dot, "operation", "DOT_PRODUCT"_s);
			_graph.Link(normal, dot, "Vector") iferr_return;
			_graph.SetInput(dot, "Vector_001", light.direction);
			towards = NodeOutput { dot, "Value"_s };
		}
		else
		{
			const maxon::Int offset = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(offset, "operation", "SUBTRACT"_s);
			_graph.SetInput(offset, "Vector", light.position);
			_graph.Link(NodeOutput { geometry, "Position"_s }, offset, "Vector_001") iferr_return;
			const maxon::Int unit = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(unit, "operation", "NORMALIZE"_s);
			_graph.Link(NodeOutput { offset, "Vector"_s }, unit, "Vector") iferr_return;
			const maxon::Int dot = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(dot, "operation", "DOT_PRODUCT"_s);
			_graph.Link(normal, dot, "Vector") iferr_return;
			_graph.Link(NodeOutput { unit, "Vector"_s }, dot, "Vector_001") iferr_return;
			towards = NodeOutput { dot, "Value"_s };
		}
		const maxon::Int lit = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(lit, "operation", "MAXIMUM"_s);
		_graph.Link(towards, lit, "Value") iferr_return;
		_graph.SetInput(lit, "Value_001", 0.0);
		const NodeOutput facing { lit, "Value"_s };
		if (cel)
		{
			if (total.IsValid())
			{
				total = MathOf2(_graph, "MAXIMUM", total, facing) iferr_return;
			}
			else
			{
				total = facing;
			}
		}
		else
		{
			const maxon::Int weighted = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(weighted, "operation", "SCALE"_s);
			_graph.SetInput(weighted, "Vector", light.color * light.brightness);
			_graph.Link(facing, weighted, "Scale") iferr_return;
			const NodeOutput part { weighted, "Vector"_s };
			if (total.IsValid())
			{
				const maxon::Int sum = _graph.Add("ShaderNodeVectorMath") iferr_return;
				_graph.SetProperty(sum, "operation", "ADD"_s);
				_graph.Link(total, sum, "Vector") iferr_return;
				_graph.Link(part, sum, "Vector_001") iferr_return;
				total = NodeOutput { sum, "Vector"_s };
			}
			else
			{
				total = part;
			}
		}
	}
	if (count > MAX_SHADING_LIGHTS)
		_exporter.Warn(FormatString("Material \"@\": in Cycles a shader that reads the lighting follows the first @ lights of the scene only.", _materialName, MAX_SHADING_LIGHTS));
	if (!total.IsValid())
	{
		if (cel)
			return Constant(_graph, 0.0);
		return Black();
	}
	return total;
}

maxon::Result<NodeOutput> ShaderTranslator::Cel(cinema::BaseShader* shader)
{
	iferr_scope;
	const maxon::Bool camera = GetBoolParameter(shader, CELSHADER_CAMERA, true);
	const maxon::Bool lights = GetBoolParameter(shader, CELSHADER_LIGHTS, false);
	if (GetBoolParameter(shader, CELSHADER_SPECULAR, false))
		_exporter.Warn(FormatString("Material \"@\": the Cel shader's Specular is not translated.", _materialName));

	NodeOutput position;
	const maxon::Bool shadows = GetBoolParameter(shader, CELSHADER_SHADOWS, false);
	if (lights)
	{
		position = LightTerm(true, shadows) iferr_return;
		if (shadows && !_exporter.IsEevee())
		{
			_exporter.Warn(FormatString("Material \"@\": in Cycles the Cel shader's bands come from the lights' directions (Cycles has no Shader to RGB): "
				"the shadows its \"Shadows\" asks for are not in them, as they are in Eevee and Cinema 4D.", _materialName));
		}
		if (shadows && _exporter.IsEevee())
		{
			const maxon::BaseArray<ShadingLight>* const all = _exporter.GetShadingLights() iferr_return;
			if (all->GetCount() > 1)
			{
				_exporter.Warn(FormatString("Material \"@\": in Eevee the Cel shader with shadows adds the lights up, counting their brightness, where Cinema 4D "
					"takes the strongest direction of any of them: with several lights the bands differ.", _materialName));
			}
		}
		if (shadows && GetIntParameter(shader, CELSHADER_SHADOWSTYPE, CELSHADER_SHADOWSTYPE_LIGHTS) != CELSHADER_SHADOWSTYPE_LIGHTS)
		{
			_exporter.Warn(FormatString("Material \"@\": the Cel shader's shadow types Color and Multiply are not translated; its shadows take the Diffuse "
				"gradient's left end.", _materialName));
		}
	}
	if (camera)
	{
		// Light from the camera: the view direction (Geometry's Incoming points back to the viewer). Measured within
		// 0.15 of Cinema 4D's own camera light only.
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		const maxon::Int dot = _graph.Add("ShaderNodeVectorMath") iferr_return;
		_graph.SetProperty(dot, "operation", "DOT_PRODUCT"_s);
		_graph.Link(NodeOutput { geometry, "Normal"_s }, dot, "Vector") iferr_return;
		_graph.Link(NodeOutput { geometry, "Incoming"_s }, dot, "Vector_001") iferr_return;
		const maxon::Int facing = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(facing, "operation", "MAXIMUM"_s);
		_graph.Link(NodeOutput { dot, "Value"_s }, facing, "Value") iferr_return;
		_graph.SetInput(facing, "Value_001", 0.0);
		const NodeOutput fromCamera { facing, "Value"_s };
		if (position.IsValid())
		{
			position = MathOf2(_graph, "MAXIMUM", position, fromCamera) iferr_return;
		}
		else
		{
			position = fromCamera;
		}
		_exporter.Warn(FormatString("Material \"@\": the Cel shader's Camera light is approximated by the view direction.", _materialName));
	}
	if (!position.IsValid())
	{
		position = Constant(_graph, 0.0) iferr_return;
	}
	return Ramp(shader, CELSHADER_DIFFUSE, position);
}

maxon::Result<NodeOutput> ShaderTranslator::Lumas(cinema::BaseShader* shader)
{
	iferr_scope;
	// The diffuse part, without contrast: its colour times its illumination times the lighting.
	if (!GetBoolParameter(shader, SLA_LUMAS_DIFFUSE_ACTIVE, true) || GetFloatParameter(shader, SLA_LUMAS_DIFFUSE_CONTRAST, 0.0) > 1e-4)
		return NodeOutput();
	if (GetBoolParameter(shader, SLA_LUMAS_SPECULAR1_ACTIVE, true) || GetBoolParameter(shader, SLA_LUMAS_SPECULAR2_ACTIVE, false) ||
		GetBoolParameter(shader, SLA_LUMAS_SPECULAR3_ACTIVE, false) || GetBoolParameter(shader, SLA_LUMAS_ANISO_ACTIVE, false))
	{
		_exporter.Warn(FormatString("Material \"@\": the Lumas shader's specular and anisotropy are not translated; its diffuse part is.", _materialName));
	}
	if (GetIntParameter(shader, SLA_LUMAS_DIFFUSE_ALGORITHM, SLA_LUMAS_DIFFUSE_ALGO_INTERNAL) != SLA_LUMAS_DIFFUSE_ALGO_INTERNAL)
		_exporter.Warn(FormatString("Material \"@\": the Lumas shader's Oren-Nayar diffuse is rendered as Lambert's.", _materialName));
	const cinema::Vector color = _exporter.GetColors().Input(GetVectorParameter(shader, SLA_LUMAS_DIFFUSE_COLOR, cinema::Vector(0.8))) *
		GetFloatParameter(shader, SLA_LUMAS_DIFFUSE_ILLUMINATION, 0.6);
	const NodeOutput lighting = LightTerm(false, true) iferr_return;
	if (!_exporter.IsEevee())
	{
		_exporter.Warn(FormatString("Material \"@\": in Cycles the Lumas shader's lighting comes from the lights' directions and brightness (Cycles has no "
			"Shader to RGB): their shadows and falloff are not in it.", _materialName));
	}
	const maxon::Int tinted = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(tinted, "operation", "MULTIPLY"_s);
	_graph.Link(lighting, tinted, "Vector") iferr_return;
	_graph.SetInput(tinted, "Vector_001", color);
	return NodeOutput { tinted, "Vector"_s };
}

maxon::Result<NodeOutput> ShaderTranslator::AmbientOcclusion(cinema::BaseShader* shader)
{
	iferr_scope;
	// Blender's Ambient Occlusion node within the shader's Maximum Ray Length: 1 where nothing is near, its
	// gradient from there. Cinema 4D's own sampling, dispersion and contrast are not Blender's (not measured).
	if (GetBoolParameter(shader, AMBIENTOCCLUSIONSHADER_REVERSE, false) || GetBoolParameter(shader, AMBIENTOCCLUSIONSHADER_ENVIRONMENT, false))
		return NodeOutput();
	const Coords coords = Coords::FromDocument(_exporter.GetDocument());
	const maxon::Int node = _graph.Add("ShaderNodeAmbientOcclusion") iferr_return;
	_graph.SetInput(node, "Distance", coords.Length(GetFloatParameter(shader, AMBIENTOCCLUSIONSHADER_MAXLENGTH, 100.0)));
	return Ramp(shader, AMBIENTOCCLUSIONSHADER_GRADIENT, NodeOutput { node, "AO"_s });
}

} // namespace cyclesrender
