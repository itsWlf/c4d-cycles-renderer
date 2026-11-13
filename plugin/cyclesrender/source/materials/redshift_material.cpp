// SPDX-License-Identifier: Apache-2.0
// The Redshift Standard Material as Principled BSDFs (redshift_material.h). What each option
// maps to is listed in docs/reference/c4d-sdk.md, "Redshift Standard Material"; the nodes wired
// into its inputs are translated by redshift_nodes.cpp.

#include "c4d.h"
#include "c4d_basematerial.h"
#include "maxon/graph.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/url.h"

#include "material_export.h"
#include "redshift_material.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."
#define STD(name) maxon::Id(RS_NODE "standardmaterial." name)

namespace cyclesrender
{

// A colour input: its value, and the output of the texture plugged into it, if any.
struct ColorInput
{
	cinema::Vector value = cinema::Vector(1.0);
	NodeOutput texture;
};

// A number input, likewise.
struct NumberInput
{
	maxon::Float64 value = 0.0;
	NodeOutput texture;
};

class RedshiftReader
{
public:
	RedshiftReader(MaterialExporter& exporter, NodeGraph& graph, const cinema::BaseMaterial* material, const maxon::GraphNode& inputs)
		: _exporter(exporter), _graph(graph), _material(material), _inputs(inputs) { }

	maxon::Result<maxon::Float64> Number(const maxon::Id& id, maxon::Float64 fallback) const
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		if (!port.IsValid())
			return fallback;
		iferr (const maxon::Opt<const maxon::Float&> value = port.GetEffectivePortValue<maxon::Float>())
			return fallback;
		return value ? maxon::Float64(*value) : fallback;
	}

	maxon::Result<maxon::Int> Integer(const maxon::Id& id, maxon::Int fallback) const
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		if (!port.IsValid())
			return fallback;
		iferr (const maxon::Opt<const maxon::Int&> value = port.GetEffectivePortValue<maxon::Int>())
			return fallback;
		return value ? *value : fallback;
	}

	maxon::Result<maxon::Bool> Switch(const maxon::Id& id, maxon::Bool fallback) const
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		if (!port.IsValid())
			return fallback;
		iferr (const maxon::Opt<const maxon::Bool&> value = port.GetEffectivePortValue<maxon::Bool>())
			return fallback;
		return value ? *value : fallback;
	}

	maxon::Result<cinema::Vector> Color(const maxon::Id& id, const cinema::Vector& fallback) const
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		return PortColor(port, fallback);
	}

	/// A colour input with the texture plugged into it. @p what names the input in a warning.
	maxon::Result<ColorInput> ColorWithTexture(const maxon::Id& id, const cinema::Vector& fallback, const maxon::Char* what)
	{
		iferr_scope;
		ColorInput input;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		input.value = PortColor(port, fallback) iferr_return;
		input.texture = Texture(port, true, what) iferr_return;
		return input;
	}

	maxon::Result<NumberInput> NumberWithTexture(const maxon::Id& id, maxon::Float64 fallback, const maxon::Char* what)
	{
		iferr_scope;
		NumberInput input;
		input.value = Number(id, fallback) iferr_return;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		input.texture = Texture(port, false, what) iferr_return;
		return input;
	}

	/// The normal a Bump Map node plugged into @p id gives, as a Bump or Normal Map node of Blender.
	maxon::Result<NodeOutput> Bump(const maxon::Id& id, const maxon::Char* what)
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		const maxon::GraphNode source = Source(port) iferr_return;
		if (!source.IsValid())
			return NodeOutput();
		const maxon::Id asset = AssetOf(source) iferr_return;
		if (asset != maxon::Id(RS_NODE "bumpmap"))
		{
			NotTranslated(source, what);
			return NodeOutput();
		}
		const maxon::GraphNode inputs = source.GetInputs() iferr_return;
		const RedshiftReader bump(_exporter, _graph, _material, inputs);
		const maxon::GraphNode input = inputs.FindChild(maxon::Id(RS_NODE "bumpmap.input")) iferr_return;
		const NodeOutput texture = Texture(input, false, what) iferr_return;
		if (!texture.IsValid())
			return NodeOutput();
		const maxon::Float64 scale = bump.Number(maxon::Id(RS_NODE "bumpmap.scale"), 1.0) iferr_return;
		const maxon::Int type = bump.Integer(maxon::Id(RS_NODE "bumpmap.inputtype"), 0) iferr_return;
		if (type == 0)
		{
			// A height field. Redshift's scale is a height in scene units; a centimetre scene is assumed.
			const maxon::Int node = _graph.Add("ShaderNodeBump") iferr_return;
			_graph.SetInput(node, "Strength", 1.0);
			_graph.SetInput(node, "Distance", scale * 0.01);
			_graph.Link(texture, node, "Height") iferr_return;
			return NodeOutput { node, "Normal"_s };
		}
		const maxon::Int node = _graph.Add("ShaderNodeNormalMap") iferr_return;
		_graph.SetProperty(node, "space", type == 1 ? "TANGENT"_s : "OBJECT"_s);
		_graph.SetInput(node, "Strength", scale);
		_graph.Link(texture, node, "Color") iferr_return;
		return NodeOutput { node, "Normal"_s };
	}

	void Warn(const maxon::String& text) const
	{
		_exporter.Warn(FormatString("Material \"@\": @", _material->GetName(), text));
	}

private:
	static maxon::Result<cinema::Vector> PortColor(const maxon::GraphNode& port, const cinema::Vector& fallback)
	{
		if (!port.IsValid())
			return fallback;
		ifnoerr (const maxon::Opt<const maxon::Color&> rgb = port.GetEffectivePortValue<maxon::Color>())
		{
			if (rgb)
				return cinema::Vector(rgb->r, rgb->g, rgb->b);
		}
		ifnoerr (const maxon::Opt<const maxon::ColorA&> rgba = port.GetEffectivePortValue<maxon::ColorA>())
		{
			if (rgba)
				return cinema::Vector(rgba->r, rgba->g, rgba->b);
		}
		return fallback;
	}

	// The node whose output is wired into the port; invalid when nothing is.
	static maxon::Result<maxon::GraphNode> Source(const maxon::GraphNode& port)
	{
		iferr_scope;
		maxon::GraphNode output;
		if (!port.IsValid())
			return output;
		port.GetConnections(maxon::PORT_DIR::INPUT, [&output](const maxon::GraphConnection& connection) -> maxon::Result<maxon::Bool>
			{
				output = connection.first;
				return false;
			}) iferr_return;
		if (!output.IsValid())
			return output;
		return output.GetAncestor(maxon::NODE_KIND::NODE);
	}

	static maxon::Result<maxon::Id> AssetOf(const maxon::GraphNode& node)
	{
		iferr_scope;
		const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
		return asset.Get<0>();
	}

	void NotTranslated(const maxon::GraphNode& node, const maxon::Char* what) const
	{
		maxon::Id asset;
		iferr (asset = AssetOf(node))
			asset = maxon::Id();
		maxon::String name = asset.ToString();
		const maxon::String prefix = maxon::String(RS_NODE);
		if (name.StartsWith(prefix))
			name = name.GetPart(prefix.GetLength(), name.GetLength() - prefix.GetLength());
		Warn(FormatString("the \"@\" node plugged into @ is not translated; the value set on the input is used.", name, maxon::String(what)));
	}

	// What is wired into the port, as Blender nodes (redshift_nodes.h): a Texture node, or a chain
	// of the nodes that have a counterpart; invalid (and named in the console) otherwise.
	maxon::Result<NodeOutput> Texture(const maxon::GraphNode& port, maxon::Bool colorData, const maxon::Char* what)
	{
		RedshiftNodes nodes(_exporter, _graph, _material);
		return nodes.Wired(port, colorData, what);
	}

	MaterialExporter& _exporter;
	NodeGraph& _graph;
	const cinema::BaseMaterial* _material;
	maxon::GraphNode _inputs;
};

static cinema::Vector Lerp(const cinema::Vector& a, const cinema::Vector& b, maxon::Float64 t)
{
	return a + (b - a) * t;
}

// a * (1 - factor) + b * factor, as a constant where both are constants.
static maxon::Result<ColorInput> MixColors(NodeGraph& graph, const ColorInput& a, const ColorInput& b, maxon::Float64 factor)
{
	iferr_scope;
	if (factor <= 0.0)
		return a;
	if (factor >= 1.0)
		return b;
	ColorInput result;
	if (!a.texture.IsValid() && !b.texture.IsValid())
	{
		result.value = Lerp(a.value, b.value, factor);
		return result;
	}
	const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
	graph.SetProperty(mix, "data_type", "RGBA"_s);
	graph.SetProperty(mix, "blend_type", "MIX"_s);
	graph.SetInput(mix, "Factor_Float", factor);
	graph.SetInput(mix, "A_Color", a.value);
	graph.SetInput(mix, "B_Color", b.value);
	if (a.texture.IsValid())
	{
		graph.Link(a.texture, mix, "A_Color") iferr_return;
	}
	if (b.texture.IsValid())
	{
		graph.Link(b.texture, mix, "B_Color") iferr_return;
	}
	result.value = Lerp(a.value, b.value, factor);
	result.texture = NodeOutput { mix, "Result_Color"_s };
	return result;
}

// The colour times a constant colour.
static maxon::Result<ColorInput> Tint(NodeGraph& graph, const ColorInput& color, const cinema::Vector& factor)
{
	iferr_scope;
	ColorInput result;
	result.value = cinema::Vector(color.value.x * factor.x, color.value.y * factor.y, color.value.z * factor.z);
	if (!color.texture.IsValid() || factor == cinema::Vector(1.0))
	{
		result.texture = color.texture;
		return result;
	}
	const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
	graph.SetProperty(mix, "data_type", "RGBA"_s);
	graph.SetProperty(mix, "blend_type", "MULTIPLY"_s);
	graph.SetInput(mix, "Factor_Float", 1.0);
	graph.SetInput(mix, "B_Color", factor);
	graph.Link(color.texture, mix, "A_Color") iferr_return;
	result.texture = NodeOutput { mix, "Result_Color"_s };
	return result;
}

static maxon::Result<void> SetColor(NodeGraph& graph, maxon::Int node, const maxon::Char* socket, const ColorInput& color)
{
	iferr_scope;
	graph.SetInput(node, socket, color.value);
	if (color.texture.IsValid())
	{
		graph.Link(color.texture, node, socket) iferr_return;
	}
	return maxon::OK;
}

// The number, times a factor when it is a constant. A texture is linked as it is: scaling one
// needs a Math node, added only where the factor matters.
static maxon::Result<void> SetNumber(NodeGraph& graph, maxon::Int node, const maxon::Char* socket, const NumberInput& number, maxon::Float64 factor = 1.0)
{
	iferr_scope;
	graph.SetInput(node, socket, number.value * factor);
	if (!number.texture.IsValid())
		return maxon::OK;
	if (factor == 1.0)
	{
		graph.Link(number.texture, node, socket) iferr_return;
		return maxon::OK;
	}
	const maxon::Int math = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(math, "operation", "MULTIPLY"_s);
	graph.SetInput(math, "Value_001", factor);
	graph.Link(number.texture, math, "Value") iferr_return;
	graph.Link(NodeOutput { math, "Value"_s }, node, socket) iferr_return;
	return maxon::OK;
}

maxon::Result<maxon::Bool> BuildRedshiftSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface)
{
	iferr_scope;

	const maxon::Id space("com.redshift3d.redshift4c4d.class.nodespace");
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	if (!nodeMaterial->HasSpace(space))
		return false;
	const maxon::nodes::NodesGraphModelRef model = nodeMaterial->GetGraph(space) iferr_return;
	if (!model)
		return false;

	// The Standard Material node wired into the output's surface.
	maxon::GraphNode standard;
	const maxon::GraphNode root = model.GetViewRoot();
	root.GetInnerNodes(maxon::NODE_KIND::NODE, false, [&standard](const maxon::GraphNode& node) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
			if (asset.Get<0>() != maxon::Id("com.redshift3d.redshift4c4d.node.output"))
				return true;
			const maxon::GraphNode inputs = node.GetInputs() iferr_return;
			const maxon::GraphNode port = inputs.FindChild(maxon::Id("com.redshift3d.redshift4c4d.node.output.surface")) iferr_return;
			if (!port.IsValid())
				return true;
			maxon::GraphNode output;
			port.GetConnections(maxon::PORT_DIR::INPUT, [&output](const maxon::GraphConnection& connection) -> maxon::Result<maxon::Bool>
				{
					output = connection.first;
					return false;
				}) iferr_return;
			if (output.IsValid())
			{
				standard = output.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
			}
			return false;
		}) iferr_return;
	if (!standard.IsValid())
		return false;
	const maxon::IdAndVersion standardAsset = standard.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
	if (standardAsset.Get<0>() != maxon::Id(RS_NODE "standardmaterial"))
		return false;

	const maxon::GraphNode inputs = standard.GetInputs() iferr_return;
	RedshiftReader read(exporter, graph, material, inputs);

	// --- read
	ColorInput baseColor = read.ColorWithTexture(STD("base_color"), cinema::Vector(0.5), "Base Color") iferr_return;
	const maxon::Float64 baseWeight = read.Number(STD("base_color_weight"), 1.0) iferr_return;
	const NumberInput diffuseRoughness = read.NumberWithTexture(STD("diffuse_roughness"), 0.0, "Diffuse Roughness") iferr_return;
	const NumberInput metalness = read.NumberWithTexture(STD("metalness"), 0.0, "Metalness") iferr_return;

	const NumberInput reflWeight = read.NumberWithTexture(STD("refl_weight"), 1.0, "Reflection Weight") iferr_return;
	const ColorInput reflColor = read.ColorWithTexture(STD("refl_color"), cinema::Vector(1.0), "Reflection Color") iferr_return;
	NumberInput reflRoughness = read.NumberWithTexture(STD("refl_roughness"), 0.2, "Reflection Roughness") iferr_return;
	const maxon::Bool reflGlossiness = read.Switch(STD("refl_isglossiness"), false) iferr_return;
	const maxon::Float64 ior = read.Number(STD("refl_ior"), 1.5) iferr_return;
	const maxon::Float64 anisotropy = read.Number(STD("refl_aniso"), 0.0) iferr_return;
	const maxon::Float64 anisotropyRotation = read.Number(STD("refl_aniso_rotation"), 0.0) iferr_return;

	const NumberInput refrWeight = read.NumberWithTexture(STD("refr_weight"), 0.0, "Transmission Weight") iferr_return;
	const ColorInput refrColor = read.ColorWithTexture(STD("refr_color"), cinema::Vector(1.0), "Transmission Color") iferr_return;
	const maxon::Float64 refrRoughness = read.Number(STD("refr_roughness"), 0.0) iferr_return;
	const maxon::Float64 refrDepth = read.Number(STD("ss_depth"), 0.0) iferr_return;
	const maxon::Float64 dispersion = read.Number(STD("refr_abbe"), 0.0) iferr_return;
	const maxon::Bool thinWalled = read.Switch(STD("refr_thin_walled"), false) iferr_return;

	const maxon::Float64 sssAmount = read.Number(STD("ms_amount"), 0.0) iferr_return;
	const ColorInput sssColor = read.ColorWithTexture(STD("ms_color"), cinema::Vector(1.0), "Subsurface Color") iferr_return;
	const cinema::Vector sssRadius = read.Color(STD("ms_radius"), cinema::Vector(1.0)) iferr_return;
	const maxon::Float64 sssScale = read.Number(STD("ms_radius_scale"), 1.0) iferr_return;
	const maxon::Float64 sssPhase = read.Number(STD("ms_phase"), 0.0) iferr_return;

	const NumberInput sheenWeight = read.NumberWithTexture(STD("sheen_weight"), 0.0, "Sheen Weight") iferr_return;
	const ColorInput sheenColor = read.ColorWithTexture(STD("sheen_color"), cinema::Vector(1.0), "Sheen Color") iferr_return;
	const maxon::Float64 sheenRoughness = read.Number(STD("sheen_roughness"), 0.3) iferr_return;

	const maxon::Float64 filmThickness = read.Number(STD("thinfilm_thickness"), 0.0) iferr_return;
	const maxon::Float64 filmIor = read.Number(STD("thinfilm_ior"), 1.5) iferr_return;

	const NumberInput coatWeight = read.NumberWithTexture(STD("coat_weight"), 0.0, "Coat Weight") iferr_return;
	const ColorInput coatColor = read.ColorWithTexture(STD("coat_color"), cinema::Vector(1.0), "Coat Color") iferr_return;
	const NumberInput coatRoughness = read.NumberWithTexture(STD("coat_roughness"), 0.0, "Coat Roughness") iferr_return;
	const maxon::Float64 coatIor = read.Number(STD("coat_ior"), 1.5) iferr_return;

	const NumberInput emissionWeight = read.NumberWithTexture(STD("emission_weight"), 0.0, "Emission Weight") iferr_return;
	ColorInput emissionColor = read.ColorWithTexture(STD("emission_color"), cinema::Vector(1.0), "Emission Color") iferr_return;

	const ColorInput opacity = read.ColorWithTexture(STD("opacity_color"), cinema::Vector(1.0), "Opacity") iferr_return;
	const cinema::Vector overall = read.Color(STD("overall_color"), cinema::Vector(1.0)) iferr_return;
	const maxon::Bool overallOnEmission = read.Switch(STD("overallaffectsemission"), false) iferr_return;

	const NodeOutput normal = read.Bump(STD("bump_input"), "Bump Map") iferr_return;
	const NodeOutput coatNormal = read.Bump(STD("coat_bump_input"), "Coat Bump Map") iferr_return;

	// --- what the Principled BSDF cannot do
	if (refrRoughness > 0.0)
		read.Warn("the transmission's own extra roughness is not translated; the reflection roughness blurs both."_s);
	if (refrDepth > 0.0)
		read.Warn("the transmission depth is not translated; the transmission colour tints the surface, whatever the thickness."_s);
	if (dispersion > 0.0)
		read.Warn("dispersion is not translated."_s);
	if (thinWalled)
		read.Warn("\"Thin Walled\" is not translated; the object refracts as a solid."_s);

	// --- build
	if (reflGlossiness)
	{
		reflRoughness.value = 1.0 - reflRoughness.value;
		if (reflRoughness.texture.IsValid())
		{
			const maxon::Int invert = graph.Add("ShaderNodeMath") iferr_return;
			graph.SetProperty(invert, "operation", "SUBTRACT"_s);
			graph.SetInput(invert, "Value", 1.0);
			graph.Link(reflRoughness.texture, invert, "Value_001") iferr_return;
			reflRoughness.texture = NodeOutput { invert, "Value"_s };
		}
	}
	if (overallOnEmission)
	{
		emissionColor = Tint(graph, emissionColor, overall) iferr_return;
	}

	// One Principled BSDF with everything but the base colour and the transmission.
	auto principled = [&](const ColorInput& color, maxon::Float64 transmission) -> maxon::Result<maxon::Int>
	{
		iferr_scope;
		const maxon::Int bsdf = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
		SetColor(graph, bsdf, "Base Color", color) iferr_return;
		graph.SetInput(bsdf, "Transmission Weight", transmission);
		SetNumber(graph, bsdf, "Diffuse Roughness", diffuseRoughness) iferr_return;
		SetNumber(graph, bsdf, "Metallic", metalness) iferr_return;

		// A reflection weight of 1 is the Principled BSDF's specular level of 0.5.
		SetNumber(graph, bsdf, "Specular IOR Level", reflWeight, 0.5) iferr_return;
		SetColor(graph, bsdf, "Specular Tint", reflColor) iferr_return;
		SetNumber(graph, bsdf, "Roughness", reflRoughness) iferr_return;
		graph.SetInput(bsdf, "IOR", maxon::Max(1.0, ior));
		graph.SetInput(bsdf, "Anisotropic", maxon::Clamp01(maxon::Abs(anisotropy)));
		// A negative anisotropy stretches the highlight the other way: a quarter turn.
		graph.SetInput(bsdf, "Anisotropic Rotation", anisotropyRotation + (anisotropy < 0.0 ? 0.25 : 0.0));

		graph.SetInput(bsdf, "Subsurface Weight", maxon::Clamp01(sssAmount));
		graph.SetInput(bsdf, "Subsurface Radius", sssRadius);
		graph.SetInput(bsdf, "Subsurface Scale", sssScale * 0.01);	// scene units, taken as centimetres
		graph.SetInput(bsdf, "Subsurface Anisotropy", maxon::Clamp01(sssPhase));

		SetNumber(graph, bsdf, "Sheen Weight", sheenWeight) iferr_return;
		SetColor(graph, bsdf, "Sheen Tint", sheenColor) iferr_return;
		graph.SetInput(bsdf, "Sheen Roughness", sheenRoughness);

		graph.SetInput(bsdf, "Thin Film Thickness", filmThickness);	// nanometres in both
		graph.SetInput(bsdf, "Thin Film IOR", maxon::Max(1.0, filmIor));

		SetNumber(graph, bsdf, "Coat Weight", coatWeight) iferr_return;
		SetColor(graph, bsdf, "Coat Tint", coatColor) iferr_return;
		SetNumber(graph, bsdf, "Coat Roughness", coatRoughness) iferr_return;
		graph.SetInput(bsdf, "Coat IOR", maxon::Max(1.0, coatIor));

		SetColor(graph, bsdf, "Emission Color", emissionColor) iferr_return;
		SetNumber(graph, bsdf, "Emission Strength", emissionWeight) iferr_return;

		graph.SetInput(bsdf, "Alpha", maxon::Clamp01((opacity.value.x + opacity.value.y + opacity.value.z) / 3.0));
		if (opacity.texture.IsValid())
		{
			graph.Link(opacity.texture, bsdf, "Alpha") iferr_return;
		}

		if (normal.IsValid())
		{
			graph.Link(normal, bsdf, "Normal") iferr_return;
		}
		if (coatNormal.IsValid())
		{
			graph.Link(coatNormal, bsdf, "Coat Normal") iferr_return;
		}
		return bsdf;
	};

	// The Principled BSDF has one colour for diffuse, subsurface and transmission, where Redshift
	// has three and layers them: transmission below, the rest on top by (1 - transmission weight).
	// So the part that transmits and the part that does not are two BSDFs, mixed by that weight.
	ColorInput opaqueColor = Tint(graph, baseColor, cinema::Vector(baseWeight)) iferr_return;
	opaqueColor = MixColors(graph, opaqueColor, sssColor, maxon::Clamp01(sssAmount)) iferr_return;
	opaqueColor = Tint(graph, opaqueColor, overall) iferr_return;
	const maxon::Bool transmits = refrWeight.texture.IsValid() || refrWeight.value > 0.0;
	const maxon::Bool onlyTransmits = !refrWeight.texture.IsValid() && refrWeight.value >= 1.0;

	NodeOutput glass;
	if (transmits)
	{
		const ColorInput glassColor = Tint(graph, refrColor, overall) iferr_return;
		const maxon::Int bsdf = principled(glassColor, 1.0) iferr_return;
		// Redshift lets light through a transmitting surface as a tinted, straight shadow; Cycles
		// would need caustics for that, which stay noisy for a long time. So for shadow rays, and
		// for the rays that gather bounced light, the surface is plainly transparent.
		const maxon::Int path = graph.Add("ShaderNodeLightPath") iferr_return;
		const maxon::Int either = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(either, "operation", "MAXIMUM"_s);
		graph.Link(NodeOutput { path, "Is Shadow Ray"_s }, either, "Value") iferr_return;
		graph.Link(NodeOutput { path, "Is Diffuse Ray"_s }, either, "Value_001") iferr_return;
		const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
		SetColor(graph, transparent, "Color", glassColor) iferr_return;
		const maxon::Int mix = graph.Add("ShaderNodeMixShader") iferr_return;
		graph.Link(NodeOutput { either, "Value"_s }, mix, "Fac") iferr_return;
		graph.Link(NodeOutput { bsdf, "BSDF"_s }, mix, 1) iferr_return;
		graph.Link(NodeOutput { transparent, "BSDF"_s }, mix, 2) iferr_return;
		glass = NodeOutput { mix, "Shader"_s };
	}
	if (onlyTransmits)
	{
		surface = glass;
		return true;
	}
	const maxon::Int opaque = principled(opaqueColor, 0.0) iferr_return;
	if (!transmits)
	{
		surface = NodeOutput { opaque, "BSDF"_s };
		return true;
	}
	const maxon::Int layers = graph.Add("ShaderNodeMixShader") iferr_return;
	graph.SetInput(layers, "Fac", maxon::Clamp01(refrWeight.value));
	if (refrWeight.texture.IsValid())
	{
		graph.Link(refrWeight.texture, layers, "Fac") iferr_return;
	}
	graph.Link(NodeOutput { opaque, "BSDF"_s }, layers, 1) iferr_return;
	graph.Link(glass, layers, 2) iferr_return;
	surface = NodeOutput { layers, "Shader"_s };
	return true;
}

} // namespace cyclesrender
