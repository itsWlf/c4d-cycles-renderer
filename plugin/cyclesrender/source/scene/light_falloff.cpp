// SPDX-License-Identifier: Apache-2.0
// The falloff and photometric intensity of standard lights (light_falloff.h). Reading order: the
// photometric factor and the falloff as TranslateStandardLight reads them, then what Eevee gets
// (FalloffForEevee), then the light node tree Cycles gets (WriteFalloffTree).
//
// Measured with the Standard renderer of Cinema 4D 2027.0.0 (internal/f13/probe_w29.py: a white plane
// under an omni, spot, area or infinite light, the pixel below it, OCIO and Basic documents alike),
// d the straight distance from the light, B its brightness, r its Radius/Decay, i its inner distance:
//   None                     B
//   Step                     B for d < r, 0 beyond
//   Linear                   B up to i, then B (r - d) / (r - i), 0 from r on
//   Inverse Square           c/cmax (cmax B (r/d)^2 - 0.01), at least 0: the 0.01 taken off the light's brightest
//                            channel cmax of its colour c, whatever B, and the colour kept
//   Inverse Square Clamped   the same with B min(1, (r/d)^2)
// Inverse and Inverse Clamped cannot be chosen in 2027; they are translated by their names (untested).

#include "c4d.h"
#include "olight.h"

#include "../materials/node_graph.h"
#include "light_falloff.h"
#include "parameters.h"

namespace cyclesrender
{

// What the Standard renderer takes off an inverse square's light, in its brightest channel (measured from
// 0.05 to 5 times the brightness and with five colours: always 0.01, the light ending where it reaches it;
// (1, 0.6, 0.3) and (0.2, 0.5, 1) kept their colour exactly, grey 0.5 lost 0.01 too).
static const maxon::Float64 INVERSE_SQUARE_OFFSET = 0.01;

static void Warn(SceneWriter* writer, const maxon::String& message)
{
	if (writer)
		writer->Warn(message);
}

maxon::Float64 PhotometricFactor(const cinema::BaseObject* light, maxon::Bool& photometric, SceneWriter* writer)
{
	photometric = GetBoolParameter(light, LIGHT_PHOTOMETRIC_UNITS, false);
	if (!photometric)
		return 1.0;
	const maxon::Float64 intensity = maxon::Max(0.0, GetFloatParameter(light, LIGHT_PHOTOMETRIC_INTENSITY, 1000.0));
	const maxon::Int32 unit = GetIntParameter(light, LIGHT_PHOTOMETRIC_UNIT, LIGHT_PHOTOMETRIC_UNIT_CANDELA);
	maxon::Float64 candela = intensity;
	if (unit == LIGHT_PHOTOMETRIC_UNIT_LUMEN)
	{
		// Measured: 1000 lm on an omni, area or infinite light lights as 1000 / (4 pi) cd; on a spot as
		// 1000 lm over the solid angle of its outer cone (60 degrees: 1188 cd, 30 degrees: 4661), whatever
		// its inner angle.
		maxon::Float64 solidAngle = 4.0 * maxon::PI;
		const maxon::Int32 type = GetIntParameter(light, LIGHT_TYPE, LIGHT_TYPE_OMNI);
		if (type == LIGHT_TYPE_SPOT || type == LIGHT_TYPE_SPOTRECT)
		{
			const maxon::Float64 outer = maxon::ClampValue(GetFloatParameter(light, LIGHT_DETAILS_OUTERANGLE, maxon::PI / 6.0), 1e-4, maxon::PI);
			solidAngle = 2.0 * maxon::PI * (1.0 - maxon::Cos(outer * 0.5));
		}
		candela = intensity / solidAngle;
	}
	else if (unit != LIGHT_PHOTOMETRIC_UNIT_CANDELA)
	{
		Warn(writer, FormatString("Light \"@\": a photometric intensity in lux is not translated; the light's own intensity is used.", light->GetName()));
		photometric = false;
		return 1.0;
	}
	// Measured: 1000 cd light as a brightness of 1 (B times cd / 1000), in every document unit.
	return candela / 1000.0;
}

maxon::Float64 ReadStandardFalloff(const cinema::BaseObject* light, const Coords& coords, maxon::Bool sun, maxon::Bool photometric,
	maxon::Float64 brightness, const cinema::Vector& color, BlenderLight& out, SceneWriter* writer)
{
	const maxon::Float64 radius = maxon::Max(coords.Length(GetFloatParameter(light, LIGHT_DETAILS_OUTERDISTANCE, 500.0)), 1e-4);
	const maxon::Int32 type = GetIntParameter(light, LIGHT_DETAILS_FALLOFF, LIGHT_DETAILS_FALLOFF_NONE);
	if (sun)
	{
		// The Standard renderer lets an infinite light fall off too, by the distance from the light's plane
		// (measured: Linear 0.75 at a quarter of the radius); Blender's sun has no distance.
		if (type != LIGHT_DETAILS_FALLOFF_NONE)
			Warn(writer, FormatString("Light \"@\": the falloff of an infinite light is not translated; Blender's sun is as bright at every distance.", light->GetName()));
		return radius;
	}

	maxon::Float64 reference = radius;
	switch (type)
	{
		case LIGHT_DETAILS_FALLOFF_NONE: out.falloff = LIGHTFALLOFF::NONE; break;
		case LIGHT_DETAILS_FALLOFF_STEP: out.falloff = LIGHTFALLOFF::STEP; break;
		case LIGHT_DETAILS_FALLOFF_LINEAR:
			out.falloff = LIGHTFALLOFF::LINEAR;
			out.falloffInner = coords.Length(maxon::Max(0.0, GetFloatParameter(light, LIGHT_DETAILS_INNERDISTANCE, 0.0)));
			break;
		case LIGHT_DETAILS_FALLOFF_INVERSE: out.falloff = LIGHTFALLOFF::INVERSE; break;
		case LIGHT_DETAILS_FALLOFF_INVERSE_CLAMPED: out.falloff = LIGHTFALLOFF::INVERSE_CLAMPED; break;
		case LIGHT_DETAILS_FALLOFF_INVERSESQUARE_CLAMPED:
		case LIGHT_DETAILS_FALLOFF_INVERSESQUARE:
		default:
			out.falloff = type == LIGHT_DETAILS_FALLOFF_INVERSESQUARE_CLAMPED ? LIGHTFALLOFF::INVERSE_SQUARE_CLAMPED : LIGHTFALLOFF::INVERSE_SQUARE;
			// Measured: with photometric intensity the inverse square falls off from 1 m whatever the
			// Radius/Decay (1000 cd: 1.0 at 1 m, 0.25 at 2 m, in centimetres, millimetres and metres).
			if (photometric)
				reference = 1.0;
			{
				const maxon::Float64 lit = brightness * maxon::Max(color.x, maxon::Max(color.y, color.z)) * reference * reference;
				out.falloffCutoff = lit > 1e-12 ? INVERSE_SQUARE_OFFSET / lit : 0.0;
			}
			break;
	}
	out.falloffRadius = reference;
	return reference;
}

// Eevee's custom distance c fades a light by this at the distance d (measured in Eevee of Blender 5.2.3:
// 0.949 at 2 m, 0.757 at 3 m, 0.349 at 4 m of c = 5 m, nothing beyond c).
static maxon::Float64 EeveeWindow(maxon::Float64 distance, maxon::Float64 cutoff)
{
	const maxon::Float64 x = distance / cutoff;
	const maxon::Float64 y = 1.0 - x * x * x * x;
	return y * y;
}

void FalloffForEevee(const cinema::BaseObject* light, BlenderLight& values, SceneWriter& writer)
{
	const maxon::Float64 r = values.falloffRadius;
	const maxon::String name = light->GetName();
	switch (values.falloff)
	{
		case LIGHTFALLOFF::STEP:
		case LIGHTFALLOFF::LINEAR:
		{
			const maxon::Bool ramp = values.falloff == LIGHTFALLOFF::LINEAR && values.falloffInner < r;
			const maxon::Float64 middle = ramp ? 0.5 * (values.falloffInner + r) : 0.5 * r;
			const maxon::Float64 level = ramp ? 0.5 : 1.0;
			// The energy matches B (r/d)^2 at r; at the middle it is to give B times the level.
			values.energy *= (middle / r) * (middle / r) * level / EeveeWindow(middle, r);
			values.eeveeCutoff = r;
			writer.Warn(FormatString("Light \"@\": Eevee's lights fall off with the square of the distance; Cinema 4D's @ falloff is matched halfway to its Radius/Decay and ends there (Cycles follows it exactly).",
				name, values.falloff == LIGHTFALLOFF::LINEAR ? "linear"_s : "Step"_s));
			break;
		}
		case LIGHTFALLOFF::NONE:
			writer.Warn(FormatString("Light \"@\": Eevee's lights fall off with the square of the distance; a light without falloff is matched at its Radius/Decay, brighter closer and darker beyond (Cycles follows it exactly).",
				name));
			break;
		case LIGHTFALLOFF::INVERSE_SQUARE_CLAMPED:
		case LIGHTFALLOFF::INVERSE:
		case LIGHTFALLOFF::INVERSE_CLAMPED:
			writer.Warn(FormatString("Light \"@\": Eevee's lights fall off with the square of the distance; this falloff is matched at its Radius/Decay (Cycles follows it exactly).", name));
			break;
		case LIGHTFALLOFF::INVERSE_SQUARE:
		case LIGHTFALLOFF::PHYSICAL:
			break;
	}
	values.falloff = LIGHTFALLOFF::PHYSICAL;
}

// A Math node: @p operation of @p a and the numbers @p b and @p c (the third input, MULTIPLY_ADD's).
static maxon::Result<NodeOutput> MathOf(NodeGraph& graph, const maxon::Char* operation, const NodeOutput& a, maxon::Float64 b, maxon::Float64 c = 0.0,
	maxon::Bool clamp = false)
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(node, "operation", maxon::String(operation));
	if (clamp)
		graph.SetProperty(node, "use_clamp", true);
	graph.Link(a, node, "Value") iferr_return;
	graph.SetInput(node, "Value_001", b);
	graph.SetInput(node, "Value_002", c);
	return NodeOutput { node, "Value"_s };
}

// A Math node multiplying two outputs.
static maxon::Result<NodeOutput> Product(NodeGraph& graph, const NodeOutput& a, const NodeOutput& b)
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(node, "operation", "MULTIPLY"_s);
	graph.Link(a, node, "Value") iferr_return;
	graph.Link(b, node, "Value_001") iferr_return;
	return NodeOutput { node, "Value"_s };
}

// The falloff's shape relative to the inverse square matched at r, as a function of d and d^2: invalid for 1.
static maxon::Result<NodeOutput> FalloffShape(NodeGraph& graph, const BlenderLight& light, const NodeOutput& distance, const NodeOutput& squared)
{
	iferr_scope;
	const maxon::Float64 r = light.falloffRadius;
	switch (light.falloff)
	{
		case LIGHTFALLOFF::NONE:
			return MathOf(graph, "MULTIPLY", squared, 1.0 / (r * r));
		case LIGHTFALLOFF::STEP:
		case LIGHTFALLOFF::LINEAR:
		{
			const NodeOutput relative = MathOf(graph, "MULTIPLY", squared, 1.0 / (r * r)) iferr_return;
			NodeOutput inside;
			if (light.falloff == LIGHTFALLOFF::LINEAR && light.falloffInner < r)
			{
				// (r - d) / (r - i), held to 0..1: 1 up to the inner distance, 0 from r on.
				const maxon::Float64 span = r - light.falloffInner;
				inside = MathOf(graph, "MULTIPLY_ADD", distance, -1.0 / span, r / span, true) iferr_return;
			}
			else
			{
				inside = MathOf(graph, "LESS_THAN", distance, r) iferr_return;
			}
			return Product(graph, relative, inside);
		}
		case LIGHTFALLOFF::INVERSE_SQUARE_CLAMPED:
		{
			const NodeOutput relative = MathOf(graph, "MULTIPLY", squared, 1.0 / (r * r)) iferr_return;
			return MathOf(graph, "MINIMUM", relative, 1.0);
		}
		case LIGHTFALLOFF::INVERSE:
			return MathOf(graph, "MULTIPLY", distance, 1.0 / r);
		case LIGHTFALLOFF::INVERSE_CLAMPED:
		{
			const NodeOutput relative = MathOf(graph, "MULTIPLY", distance, 1.0 / r) iferr_return;
			return MathOf(graph, "MINIMUM", relative, 1.0);
		}
		case LIGHTFALLOFF::INVERSE_SQUARE:
		case LIGHTFALLOFF::PHYSICAL:
			break;
	}
	return NodeOutput();
}

static maxon::Result<void> BuildFalloffTree(NodeGraph& graph, const BlenderLight& light)
{
	iferr_scope;
	// Measured in Cycles of Blender 5.2.3: in a light's node tree the Light Path node's Ray Length is the
	// distance from the light (its sampled point, for an area light) to what it lights; a ramp from it
	// gave 0.8, 0.6, 0.4, 0.2 of the light at 1 to 4 m of 5. Eevee ignores the tree.
	const maxon::Int path = graph.Add("ShaderNodeLightPath") iferr_return;
	const NodeOutput distance { path, "Ray Length"_s };
	const maxon::Int square = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(square, "operation", "MULTIPLY"_s);
	graph.Link(distance, square, "Value") iferr_return;
	graph.Link(distance, square, "Value_001") iferr_return;
	const NodeOutput squared { square, "Value"_s };
	const NodeOutput shape = FalloffShape(graph, light, distance, squared) iferr_return;

	const maxon::Int emission = graph.Add("ShaderNodeEmission") iferr_return;
	const maxon::Int output = graph.Add("ShaderNodeOutputLight") iferr_return;
	graph.Link(NodeOutput { emission, "Emission"_s }, output, "Surface") iferr_return;
	graph.SetInput(emission, "Color", cinema::Vector(1.0));
	graph.SetInput(emission, "Strength", 1.0);
	NodeOutput strength = shape;
	if (light.falloffCutoff > 0.0)
	{
		// The shape (at most 1 for the two inverse squares) less d^2 times the 0.01 relative to B c r^2,
		// held to 0..1.
		const maxon::Int less = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(less, "operation", "MULTIPLY_ADD"_s);
		graph.SetProperty(less, "use_clamp", true);
		graph.Link(squared, less, "Value") iferr_return;
		graph.SetInput(less, "Value_001", -light.falloffCutoff);
		if (shape.IsValid())
		{
			graph.Link(shape, less, "Value_002") iferr_return;
		}
		else
		{
			graph.SetInput(less, "Value_002", 1.0);
		}
		strength = NodeOutput { less, "Value"_s };
	}
	if (strength.IsValid())
	{
		graph.Link(strength, emission, "Strength") iferr_return;
	}
	return maxon::OK;
}

void WriteFalloffTree(JsonWriter& json, const BlenderLight& light)
{
	if (light.falloff == LIGHTFALLOFF::PHYSICAL || light.type == BLENDERLIGHT::SUN)
		return;
	if (light.falloff == LIGHTFALLOFF::INVERSE_SQUARE && light.falloffCutoff <= 0.0)
		return;
	NodeGraph graph;
	iferr (BuildFalloffTree(graph, light))
	{
		return;	// out of memory: the light without its falloff
	}
	graph.Write(json);
}

} // namespace cyclesrender
