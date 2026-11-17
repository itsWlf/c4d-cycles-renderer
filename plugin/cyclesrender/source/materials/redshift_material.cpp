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

#include "../sdk_compat.h"
#include "../scene/coords.h"
#include "material_export.h"
#include "redshift_material.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

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

	/// The normal what is plugged into @p id gives: a Bump Map, the old Normal Map, Round Corners or
	/// a Bump Blender, as Blender's Bump, Normal Map and Bevel nodes.
	maxon::Result<NodeOutput> Bump(const maxon::Id& id, const maxon::Char* what)
	{
		iferr_scope;
		const maxon::GraphNode port = _inputs.FindChild(id) iferr_return;
		const maxon::GraphNode source = Source(port) iferr_return;
		if (!source.IsValid())
			return NodeOutput();
		return BumpOf(source, what, 0);
	}

	maxon::Result<NodeOutput> BumpOf(const maxon::GraphNode& source, const maxon::Char* what, maxon::Int depth)
	{
		iferr_scope;
		const maxon::Id asset = AssetOf(source) iferr_return;
		const maxon::GraphNode inputs = source.GetInputs() iferr_return;
		const RedshiftReader node(_exporter, _graph, _material, inputs);
		if (depth > 8)
		{
			Warn(FormatString("the bump nodes plugged into @ are nested too deeply; there is no bump.", maxon::String(what)));
			return NodeOutput();
		}
		if (asset == maxon::Id(RS_NODE "bumpmap"))
		{
			const maxon::Bool enabled = node.Switch(maxon::Id(RS_NODE "bumpmap.inuse"), true) iferr_return;
			if (!enabled)
				return NodeOutput();
			const maxon::GraphNode input = inputs.FindChild(maxon::Id(RS_NODE "bumpmap.input")) iferr_return;
			const NodeOutput texture = Texture(input, false, what) iferr_return;
			if (!texture.IsValid())
				return NodeOutput();
			const maxon::Float64 scale = node.Number(maxon::Id(RS_NODE "bumpmap.scale"), 1.0) iferr_return;
			const maxon::Int type = node.Integer(maxon::Id(RS_NODE "bumpmap.inputtype"), 0) iferr_return;
			if (type == 0)
			{
				// A height field. Redshift's scale is a height in scene units; a centimetre scene is assumed.
				const maxon::Int bump = _graph.Add("ShaderNodeBump") iferr_return;
				_graph.SetInput(bump, "Strength", 1.0);
				_graph.SetInput(bump, "Distance", scale * 0.01);
				_graph.Link(texture, bump, "Height") iferr_return;
				return NodeOutput { bump, "Normal"_s };
			}
			const maxon::Bool flip = node.Switch(maxon::Id(RS_NODE "bumpmap.flipy"), false) iferr_return;
			const maxon::Bool unbiased = node.Switch(maxon::Id(RS_NODE "bumpmap.unbiasednormalmap"), false) iferr_return;
			return NormalMap(texture, type == 1, scale, flip, unbiased);
		}
		if (asset == maxon::Id(RS_NODE "normalmap"))
		{
			// The older Normal Map node, with a file of its own (a tangent-space map).
			RedshiftNodes nodes(_exporter, _graph, _material);
			const maxon::String image = nodes.FileImage(source, "normalmap"_s, false, what, "Normal Map") iferr_return;
			if (image.IsEmpty())
				return NodeOutput();
			const maxon::Int texture = _graph.Add("ShaderNodeTexImage") iferr_return;
			_graph.SetProperty(texture, "image", image);
			const maxon::Float64 scale = node.Number(maxon::Id(RS_NODE "normalmap.scale"), 1.0) iferr_return;
			const maxon::Bool flip = node.Switch(maxon::Id(RS_NODE "normalmap.flipy"), false) iferr_return;
			const maxon::Bool unbiased = node.Switch(maxon::Id(RS_NODE "normalmap.unbiasednormalmap"), false) iferr_return;
			return NormalMap(NodeOutput { texture, "Color"_s }, true, scale, flip, unbiased);
		}
		if (asset == maxon::Id(RS_NODE "roundcorners"))
		{
			// Blender's Bevel node (Cycles only; Eevee keeps the plain normal). The radius is in document units.
			const maxon::Float64 radius = node.Number(maxon::Id(RS_NODE "roundcorners.radius"), 0.5) iferr_return;
			const maxon::Int samples = node.Integer(maxon::Id(RS_NODE "roundcorners.numsamples"), 6) iferr_return;
			const maxon::Bool sameObject = node.Switch(maxon::Id(RS_NODE "roundcorners.sameobjectonly"), false) iferr_return;
			if (!sameObject)
				Warn(FormatString("a Round Corners node plugged into @ also rounds where other objects meet the surface; Blender's Bevel rounds the object's own edges only.", maxon::String(what)));
			const maxon::Int bevel = _graph.Add("ShaderNodeBevel") iferr_return;
			_graph.SetInput(bevel, "Radius", Coords::FromDocument(_exporter.GetDocument()).Length(radius));
			_graph.SetPropertyJson(bevel, "samples", FormatString("@", maxon::ClampValue(samples, maxon::Int(1), maxon::Int(128))));
			return NodeOutput { bevel, "Normal"_s };
		}
		if (asset == maxon::Id(RS_NODE "bumpblender"))
		{
			// The base normal, then each layer mixed in by its weight (or added to it in additive mode),
			// the result normalised. Redshift blends bump vectors; blending the normals is close for
			// gentle bumps.
			const maxon::Bool additive = node.Switch(maxon::Id(RS_NODE "bumpblender.additive"), false) iferr_return;
			const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
			const NodeOutput plain { geometry, "Normal"_s };
			NodeOutput result = plain;
			const maxon::GraphNode basePort = inputs.FindChild(maxon::Id(RS_NODE "bumpblender.baseinput")) iferr_return;
			const maxon::GraphNode baseSource = Source(basePort) iferr_return;
			if (baseSource.IsValid())
			{
				const NodeOutput base = BumpOf(baseSource, what, depth + 1) iferr_return;
				if (base.IsValid())
					result = base;
			}
			static const maxon::Char* const LAYERS[3][2] = {
				{ RS_NODE "bumpblender.bumpinput0", RS_NODE "bumpblender.bumpweight0" },
				{ RS_NODE "bumpblender.bumpinput1", RS_NODE "bumpblender.bumpweight1" },
				{ RS_NODE "bumpblender.bumpinput2", RS_NODE "bumpblender.bumpweight2" } };
			for (const auto& layer : LAYERS)
			{
				const maxon::Float64 weight = node.Number(maxon::Id(layer[1]), 0.0) iferr_return;
				const maxon::GraphNode layerPort = inputs.FindChild(maxon::Id(layer[0])) iferr_return;
				const maxon::GraphNode layerSource = Source(layerPort) iferr_return;
				if (weight == 0.0 || !layerSource.IsValid())
					continue;
				const NodeOutput normal = BumpOf(layerSource, what, depth + 1) iferr_return;
				if (!normal.IsValid())
					continue;
				// Additive: result + w (n - plain); otherwise result + w (n - result).
				const maxon::Int difference = _graph.Add("ShaderNodeVectorMath") iferr_return;
				_graph.SetProperty(difference, "operation", "SUBTRACT"_s);
				_graph.Link(normal, difference, "Vector") iferr_return;
				_graph.Link(additive ? plain : result, difference, "Vector_001") iferr_return;
				const maxon::Int mix = _graph.Add("ShaderNodeVectorMath") iferr_return;
				_graph.SetProperty(mix, "operation", "MULTIPLY_ADD"_s);
				_graph.Link(NodeOutput { difference, "Vector"_s }, mix, "Vector") iferr_return;
				_graph.SetInput(mix, "Vector_001", cinema::Vector(weight));
				_graph.Link(result, mix, "Vector_002") iferr_return;
				const maxon::Int normalised = _graph.Add("ShaderNodeVectorMath") iferr_return;
				_graph.SetProperty(normalised, "operation", "NORMALIZE"_s);
				_graph.Link(NodeOutput { mix, "Vector"_s }, normalised, "Vector") iferr_return;
				result = NodeOutput { normalised, "Vector"_s };
			}
			return result;
		}
		if (asset == maxon::Id(RS_NODE "flakes"))
		{
			// The surface's normal tilted per flake (a look-alike, redshift_lookalikes.cpp).
			RedshiftNodes nodes(_exporter, _graph, _material);
			return nodes.FlakesNormal(source, what);
		}
		NotTranslated(source, what);
		return NodeOutput();
	}

	// A normal map's colour as Blender's Normal Map node: tangent or object space, the green channel
	// inverted for "Flip Y", and an unbiased map (components -1..1) moved into Blender's 0..1.
	maxon::Result<NodeOutput> NormalMap(const NodeOutput& color, maxon::Bool tangent, maxon::Float64 strength, maxon::Bool flip, maxon::Bool unbiased)
	{
		iferr_scope;
		NodeOutput value = color;
		if (unbiased)
		{
			const maxon::Int shift = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(shift, "operation", "MULTIPLY_ADD"_s);
			_graph.Link(value, shift, "Vector") iferr_return;
			_graph.SetInput(shift, "Vector_001", cinema::Vector(0.5));
			_graph.SetInput(shift, "Vector_002", cinema::Vector(0.5));
			value = NodeOutput { shift, "Vector"_s };
		}
		if (flip)
		{
			// (r, 1 - g, b)
			const maxon::Int turn = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(turn, "operation", "MULTIPLY_ADD"_s);
			_graph.Link(value, turn, "Vector") iferr_return;
			_graph.SetInput(turn, "Vector_001", cinema::Vector(1.0, -1.0, 1.0));
			_graph.SetInput(turn, "Vector_002", cinema::Vector(0.0, 1.0, 0.0));
			value = NodeOutput { turn, "Vector"_s };
		}
		const maxon::Int node = _graph.Add("ShaderNodeNormalMap") iferr_return;
		_graph.SetProperty(node, "space", tangent ? "TANGENT"_s : "OBJECT"_s);
		_graph.SetInput(node, "Strength", strength);
		_graph.Link(value, node, "Color") iferr_return;
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
		const maxon::IdAndVersion asset = OptOrDefault(node.GetValue<decltype(maxon::nodes::AssetId)>()) iferr_return;
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

// The Standard Material's ports, and the OpenPBR Material's in the same roles (nullptr: it has none).
struct SurfacePorts
{
	const maxon::Char* asset;
	const maxon::Char* baseColor; const maxon::Char* baseWeight; const maxon::Char* diffuseRoughness; const maxon::Char* metalness;
	const maxon::Char* reflWeight; const maxon::Char* reflColor; const maxon::Char* reflRoughness; const maxon::Char* reflGlossiness;
	const maxon::Char* reflIor; const maxon::Char* aniso; const maxon::Char* anisoRotation;
	const maxon::Char* refrWeight; const maxon::Char* refrColor; const maxon::Char* refrRoughness; const maxon::Char* refrDepth;
	const maxon::Char* refrAbbe; const maxon::Char* thinWalled;
	const maxon::Char* sssAmount; const maxon::Char* sssColor; const maxon::Char* sssRadius; const maxon::Char* sssScale; const maxon::Char* sssPhase;
	const maxon::Char* sheenWeight; const maxon::Char* sheenColor; const maxon::Char* sheenRoughness;
	const maxon::Char* filmThickness; const maxon::Char* filmIor;
	const maxon::Char* coatWeight; const maxon::Char* coatColor; const maxon::Char* coatRoughness; const maxon::Char* coatIor;
	const maxon::Char* emissionWeight; const maxon::Char* emissionColor;
	const maxon::Char* opacity; const maxon::Char* overall; const maxon::Char* overallOnEmission;
	const maxon::Char* bump; const maxon::Char* coatBump;
};

#define SM RS_NODE "standardmaterial."
static const SurfacePorts STANDARD_PORTS =
{
	RS_NODE "standardmaterial",
	SM "base_color", SM "base_color_weight", SM "diffuse_roughness", SM "metalness",
	SM "refl_weight", SM "refl_color", SM "refl_roughness", SM "refl_isglossiness",
	SM "refl_ior", SM "refl_aniso", SM "refl_aniso_rotation",
	SM "refr_weight", SM "refr_color", SM "refr_roughness", SM "ss_depth",
	SM "refr_abbe", SM "refr_thin_walled",
	SM "ms_amount", SM "ms_color", SM "ms_radius", SM "ms_radius_scale", SM "ms_phase",
	SM "sheen_weight", SM "sheen_color", SM "sheen_roughness",
	SM "thinfilm_thickness", SM "thinfilm_ior",
	SM "coat_weight", SM "coat_color", SM "coat_roughness", SM "coat_ior",
	SM "emission_weight", SM "emission_color",
	SM "opacity_color", SM "overall_color", SM "overallaffectsemission",
	SM "bump_input", SM "coat_bump_input"
};
#undef SM

// OpenPBR: the same lobes under OpenPBR's names. Its subsurface radius is a number and its radius
// scale a colour (the Standard Material's are the other way round); its thin film is in micrometres
// with a weight; its emission a luminance in nits; its opacity a number.
#define OP RS_NODE "openpbrmaterial."
static const SurfacePorts OPENPBR_PORTS =
{
	RS_NODE "openpbrmaterial",
	OP "base_color", OP "base_weight", OP "base_diffuse_roughness", OP "base_metalness",
	OP "specular_weight", OP "specular_color", OP "specular_roughness", nullptr,
	OP "specular_ior", OP "specular_roughness_anisotropy", nullptr,
	OP "transmission_weight", OP "transmission_color", nullptr, OP "transmission_depth",
	OP "transmission_dispersion_abbe_number", OP "geometry_thin_walled",
	OP "subsurface_weight", OP "subsurface_color", OP "subsurface_radius_scale", OP "subsurface_radius", OP "subsurface_scatter_anisotropy",
	OP "fuzz_weight", OP "fuzz_color", OP "fuzz_roughness",
	OP "thin_film_thickness", OP "thin_film_ior",
	OP "coat_weight", OP "coat_color", OP "coat_roughness", OP "coat_ior",
	OP "emission_luminance", OP "emission_color",
	OP "geometry_opacity", nullptr, nullptr,
	OP "geometry_normal", OP "geometry_coat_normal"
};
#undef OP

// An id of the table, or one that names no port when the material has none.
static maxon::Id PortId(const maxon::Char* name)
{
	return maxon::Id(name ? name : RS_NODE "none");
}

static maxon::Result<maxon::Bool> BuildSurfaceNode(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, const maxon::GraphNode& node,
	NodeOutput& surface, maxon::Int depth);

// The Standard Material or the OpenPBR Material as Principled BSDFs.
static maxon::Result<maxon::Bool> BuildStandardSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, const maxon::GraphNode& standard,
	const SurfacePorts& ports, NodeOutput& surface)
{
	iferr_scope;
	const maxon::Bool openPbr = &ports == &OPENPBR_PORTS;
	const maxon::GraphNode inputs = standard.GetInputs() iferr_return;
	RedshiftReader read(exporter, graph, material, inputs);

	// --- read
	ColorInput baseColor = read.ColorWithTexture(PortId(ports.baseColor), cinema::Vector(0.5), "Base Color") iferr_return;
	const maxon::Float64 baseWeight = read.Number(PortId(ports.baseWeight), 1.0) iferr_return;
	const NumberInput diffuseRoughness = read.NumberWithTexture(PortId(ports.diffuseRoughness), 0.0, "Diffuse Roughness") iferr_return;
	const NumberInput metalness = read.NumberWithTexture(PortId(ports.metalness), 0.0, "Metalness") iferr_return;

	const NumberInput reflWeight = read.NumberWithTexture(PortId(ports.reflWeight), 1.0, "Reflection Weight") iferr_return;
	const ColorInput reflColor = read.ColorWithTexture(PortId(ports.reflColor), cinema::Vector(1.0), "Reflection Color") iferr_return;
	NumberInput reflRoughness = read.NumberWithTexture(PortId(ports.reflRoughness), 0.2, "Reflection Roughness") iferr_return;
	const maxon::Bool reflGlossiness = read.Switch(PortId(ports.reflGlossiness), false) iferr_return;
	const maxon::Float64 ior = read.Number(PortId(ports.reflIor), 1.5) iferr_return;
	const maxon::Float64 anisotropy = read.Number(PortId(ports.aniso), 0.0) iferr_return;
	const maxon::Float64 anisotropyRotation = read.Number(PortId(ports.anisoRotation), 0.0) iferr_return;

	const NumberInput refrWeight = read.NumberWithTexture(PortId(ports.refrWeight), 0.0, "Transmission Weight") iferr_return;
	const ColorInput refrColor = read.ColorWithTexture(PortId(ports.refrColor), cinema::Vector(1.0), "Transmission Color") iferr_return;
	const maxon::Float64 refrRoughness = read.Number(PortId(ports.refrRoughness), 0.0) iferr_return;
	const maxon::Float64 refrDepth = read.Number(PortId(ports.refrDepth), 0.0) iferr_return;
	maxon::Float64 dispersion = read.Number(PortId(ports.refrAbbe), 0.0) iferr_return;
	const maxon::Bool thinWalled = read.Switch(PortId(ports.thinWalled), false) iferr_return;

	const maxon::Float64 sssAmount = read.Number(PortId(ports.sssAmount), 0.0) iferr_return;
	const ColorInput sssColor = read.ColorWithTexture(PortId(ports.sssColor), cinema::Vector(1.0), "Subsurface Color") iferr_return;
	const cinema::Vector sssRadius = read.Color(PortId(ports.sssRadius), cinema::Vector(1.0)) iferr_return;
	const maxon::Float64 sssScale = read.Number(PortId(ports.sssScale), 1.0) iferr_return;
	const maxon::Float64 sssPhase = read.Number(PortId(ports.sssPhase), 0.0) iferr_return;

	const NumberInput sheenWeight = read.NumberWithTexture(PortId(ports.sheenWeight), 0.0, "Sheen Weight") iferr_return;
	const ColorInput sheenColor = read.ColorWithTexture(PortId(ports.sheenColor), cinema::Vector(1.0), "Sheen Color") iferr_return;
	const maxon::Float64 sheenRoughness = read.Number(PortId(ports.sheenRoughness), 0.3) iferr_return;

	maxon::Float64 filmThickness = read.Number(PortId(ports.filmThickness), 0.0) iferr_return;
	const maxon::Float64 filmIor = read.Number(PortId(ports.filmIor), 1.5) iferr_return;

	const NumberInput coatWeight = read.NumberWithTexture(PortId(ports.coatWeight), 0.0, "Coat Weight") iferr_return;
	const ColorInput coatColor = read.ColorWithTexture(PortId(ports.coatColor), cinema::Vector(1.0), "Coat Color") iferr_return;
	const NumberInput coatRoughness = read.NumberWithTexture(PortId(ports.coatRoughness), 0.0, "Coat Roughness") iferr_return;
	const maxon::Float64 coatIor = read.Number(PortId(ports.coatIor), 1.5) iferr_return;

	NumberInput emissionWeight = read.NumberWithTexture(PortId(ports.emissionWeight), 0.0, "Emission Weight") iferr_return;
	ColorInput emissionColor = read.ColorWithTexture(PortId(ports.emissionColor), cinema::Vector(1.0), "Emission Color") iferr_return;

	ColorInput opacity;
	if (openPbr)
	{
		// A number in OpenPBR.
		const NumberInput number = read.NumberWithTexture(PortId(ports.opacity), 1.0, "Opacity") iferr_return;
		opacity.value = cinema::Vector(number.value);
		opacity.texture = number.texture;
	}
	else
	{
		opacity = read.ColorWithTexture(PortId(ports.opacity), cinema::Vector(1.0), "Opacity") iferr_return;
	}
	const cinema::Vector overall = read.Color(PortId(ports.overall), cinema::Vector(1.0)) iferr_return;
	const maxon::Bool overallOnEmission = read.Switch(PortId(ports.overallOnEmission), false) iferr_return;

	const NodeOutput normal = read.Bump(PortId(ports.bump), "Bump Map") iferr_return;
	const NodeOutput coatNormal = read.Bump(PortId(ports.coatBump), "Coat Bump Map") iferr_return;

	if (openPbr)
	{
		// Thin film in micrometres, only with a weight; dispersion only with its weight; emission
		// luminance in nits: Redshift renders 683 nits as 1 (measured: 1000 nits of white render 1.465).
		const maxon::Float64 filmWeight = read.Number(maxon::Id(RS_NODE "openpbrmaterial.thin_film_weight"), 0.0) iferr_return;
		filmThickness = filmWeight > 0.0 ? filmThickness * 1000.0 : 0.0;
		if (filmWeight > 0.0 && filmWeight < 1.0)
			read.Warn("the thin film's weight between 0 and 1 is not translated; the film covers the whole surface."_s);
		const maxon::Float64 dispersionWeight = read.Number(maxon::Id(RS_NODE "openpbrmaterial.transmission_dispersion_scale"), 0.0) iferr_return;
		if (dispersionWeight <= 0.0)
			dispersion = 0.0;
		emissionWeight.value /= 683.0;
		if (emissionWeight.texture.IsValid())
		{
			const maxon::Int nits = graph.Add("ShaderNodeMath") iferr_return;
			graph.SetProperty(nits, "operation", "DIVIDE"_s);
			graph.Link(emissionWeight.texture, nits, "Value") iferr_return;
			graph.SetInput(nits, "Value_001", 683.0);
			emissionWeight.texture = NodeOutput { nits, "Value"_s };
		}
		const cinema::Vector scatter = read.Color(maxon::Id(RS_NODE "openpbrmaterial.transmission_scatter"), cinema::Vector(0.0)) iferr_return;
		if (scatter != cinema::Vector(0.0))
			read.Warn("the transmission's scattering is not translated."_s);
		const maxon::Float64 coatDarkening = read.Number(maxon::Id(RS_NODE "openpbrmaterial.coat_darkening"), 1.0) iferr_return;
		if (coatDarkening > 0.0 && (coatWeight.value > 0.0 || coatWeight.texture.IsValid()))
			read.Warn("the coat's darkening of the base is not translated; the base under the coat renders brighter than in Redshift."_s);
		const maxon::Float64 coatAniso = read.Number(maxon::Id(RS_NODE "openpbrmaterial.coat_roughness_anisotropy"), 0.0) iferr_return;
		if (coatAniso != 0.0)
			read.Warn("the coat's anisotropy is not translated."_s);
	}

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

// The node wired into the Redshift Output's surface.
static maxon::Result<maxon::GraphNode> OutputSurface(const maxon::nodes::NodesGraphModelRef& model)
{
	iferr_scope;
	maxon::GraphNode found;
	const maxon::GraphNode root = model.GetViewRoot();
	root.GetInnerNodes(maxon::NODE_KIND::NODE, false, [&found](const maxon::GraphNode& node) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			const maxon::IdAndVersion asset = OptOrDefault(node.GetValue<decltype(maxon::nodes::AssetId)>()) iferr_return;
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
				found = output.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
			}
			return false;
		}) iferr_return;
	return found;
}

static maxon::Result<maxon::GraphNode> WiredNode(const maxon::GraphNode& node, const maxon::Id& port)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	const maxon::GraphNode input = inputs.FindChild(port) iferr_return;
	return RedshiftNodes::SourceNode(input);
}

// Incandescent: colour (or a black body's) times its radiance, as an Emission shader (measured: 1
// renders 1); the ray multipliers by ray type; its alpha lets what is behind show through.
static maxon::Result<maxon::Bool> BuildIncandescent(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, const maxon::GraphNode& node,
	NodeOutput& surface)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	RedshiftReader read(exporter, graph, material, inputs);
#define IN(name) maxon::Id(RS_NODE "incandescent." name)
	const maxon::Int mode = read.Integer(IN("colormode"), 0) iferr_return;
	const ColorInput color = read.ColorWithTexture(IN("color"), cinema::Vector(1.0), "Color") iferr_return;
	const NumberInput intensity = read.NumberWithTexture(IN("intensity"), 1.0, "Radiance") iferr_return;
	const maxon::Float64 temperature = read.Number(IN("temperature"), 6500.0) iferr_return;
	const NumberInput alpha = read.NumberWithTexture(IN("alpha"), 1.0, "Alpha") iferr_return;
	const maxon::Bool doubleSided = read.Switch(IN("doublesided"), true) iferr_return;
	const maxon::Float64 reflectScale = read.Number(IN("reflectscale"), 1.0) iferr_return;
	const maxon::Float64 refractScale = read.Number(IN("refractscale"), 1.0) iferr_return;
	const maxon::Float64 giScale = read.Number(IN("giscale"), 1.0) iferr_return;
	const maxon::Int alphaMode = read.Integer(IN("alphamode"), 1) iferr_return;
	const maxon::Bool additive = read.Switch(IN("additive_blend"), false) iferr_return;
#undef IN
	if (alphaMode == 0)
		read.Warn("the Incandescent material's alpha replaces the picture's alpha in Redshift; here it lets what is behind show through."_s);
	if (additive)
		read.Warn("the Incandescent material's additive blend is not translated."_s);
	const maxon::Int emission = graph.Add("ShaderNodeEmission") iferr_return;
	if (mode == 1)
	{
		const maxon::Int body = graph.Add("ShaderNodeBlackbody") iferr_return;
		graph.SetInput(body, "Temperature", temperature);
		graph.Link(NodeOutput { body, "Color"_s }, emission, "Color") iferr_return;
	}
	else
	{
		SetColor(graph, emission, "Color", color) iferr_return;
	}
	NodeOutput strength;
	graph.SetInput(emission, "Strength", intensity.value);
	if (intensity.texture.IsValid())
		strength = intensity.texture;
	if (reflectScale != 1.0 || refractScale != 1.0 || giScale != 1.0)
	{
		// 1 + (r - 1) glossy + (t - 1) transmission + (g - 1) diffuse rays, times the radiance.
		const maxon::Int path = graph.Add("ShaderNodeLightPath") iferr_return;
		NodeOutput factor;
		const struct { const maxon::Char* ray; maxon::Float64 scale; } rays[] = {
			{ "Is Glossy Ray", reflectScale }, { "Is Transmission Ray", refractScale }, { "Is Diffuse Ray", giScale } };
		for (const auto& ray : rays)
		{
			if (ray.scale == 1.0)
				continue;
			const maxon::Int add = graph.Add("ShaderNodeMath") iferr_return;
			graph.SetProperty(add, "operation", "MULTIPLY_ADD"_s);
			graph.Link(NodeOutput { path, maxon::String(ray.ray) }, add, "Value") iferr_return;
			graph.SetInput(add, "Value_001", ray.scale - 1.0);
			if (factor.IsValid())
			{
				graph.Link(factor, add, "Value_002") iferr_return;
			}
			else
			{
				graph.SetInput(add, "Value_002", 1.0);
			}
			factor = NodeOutput { add, "Value"_s };
		}
		const maxon::Int times = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(times, "operation", "MULTIPLY"_s);
		graph.Link(factor, times, "Value") iferr_return;
		graph.SetInput(times, "Value_001", intensity.value);
		if (strength.IsValid())
		{
			graph.Link(strength, times, "Value_001") iferr_return;
		}
		strength = NodeOutput { times, "Value"_s };
	}
	if (strength.IsValid())
	{
		graph.Link(strength, emission, "Strength") iferr_return;
	}
	NodeOutput result { emission, "Emission"_s };
	if (!doubleSided)
	{
		// The back emits nothing.
		const maxon::Int geometry = graph.Add("ShaderNodeNewGeometry") iferr_return;
		const maxon::Int black = graph.Add("ShaderNodeBsdfDiffuse") iferr_return;
		graph.SetInput(black, "Color", cinema::Vector(0.0));
		const maxon::Int sides = graph.Add("ShaderNodeMixShader") iferr_return;
		graph.Link(NodeOutput { geometry, "Backfacing"_s }, sides, "Fac") iferr_return;
		graph.Link(result, sides, 1) iferr_return;
		graph.Link(NodeOutput { black, "BSDF"_s }, sides, 2) iferr_return;
		result = NodeOutput { sides, "Shader"_s };
	}
	if (alpha.texture.IsValid() || alpha.value < 1.0)
	{
		const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
		const maxon::Int mix = graph.Add("ShaderNodeMixShader") iferr_return;
		graph.SetInput(mix, "Fac", maxon::Clamp01(alpha.value));
		if (alpha.texture.IsValid())
		{
			graph.Link(alpha.texture, mix, "Fac") iferr_return;
		}
		graph.Link(NodeOutput { transparent, "BSDF"_s }, mix, 1) iferr_return;
		graph.Link(result, mix, 2) iferr_return;
		result = NodeOutput { mix, "Shader"_s };
	}
	surface = result;
	return true;
}

// Sprite: the material wired into it where its stencil file reaches the threshold, transparent
// elsewhere (measured: a hard cut at the threshold on the file's own values, which are read as
// data); the stencil follows the UV map, tiled by its repeats.
static maxon::Result<maxon::Bool> BuildSprite(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, const maxon::GraphNode& node,
	NodeOutput& surface, maxon::Int depth)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	RedshiftReader read(exporter, graph, material, inputs);
	const maxon::GraphNode inner = WiredNode(node, maxon::Id(RS_NODE "sprite.input")) iferr_return;
	NodeOutput shown;
	if (inner.IsValid())
	{
		const maxon::Bool built = BuildSurfaceNode(exporter, graph, material, inner, shown, depth + 1) iferr_return;
		if (!built)
			return false;
	}
	else
	{
		read.Warn("the Sprite material has no material wired into it; it renders black where the stencil is opaque."_s);
		const maxon::Int black = graph.Add("ShaderNodeBsdfDiffuse") iferr_return;
		graph.SetInput(black, "Color", cinema::Vector(0.0));
		shown = NodeOutput { black, "BSDF"_s };
	}
	RedshiftNodes nodes(exporter, graph, material);
	const maxon::String image = nodes.FileImage(node, "sprite"_s, false, "Sprite", "Sprite") iferr_return;
	if (image.IsEmpty())
	{
		surface = shown;
		return true;
	}
	const maxon::Int mode = read.Integer(maxon::Id(RS_NODE "sprite.mode"), 0) iferr_return;
	const maxon::Float64 threshold = read.Number(maxon::Id(RS_NODE "sprite.threshold"), 0.5) iferr_return;
	const maxon::GraphNode repeatsPort = inputs.FindChild(maxon::Id(RS_NODE "sprite.repeats")) iferr_return;
	maxon::Vector2d repeats(1.0);
	if (repeatsPort.IsValid())
	{
		ifnoerr (const maxon::Opt<const maxon::Vector2d&> value = repeatsPort.GetEffectivePortValue<maxon::Vector2d>())
		{
			if (value)
				repeats = *value;
		}
	}
	const maxon::Int texture = graph.Add("ShaderNodeTexImage") iferr_return;
	graph.SetProperty(texture, "image", image);
	if (repeats != maxon::Vector2d(1.0))
	{
		const maxon::Int coordinates = graph.Add("ShaderNodeTexCoord") iferr_return;
		const maxon::Int mapping = graph.Add("ShaderNodeMapping") iferr_return;
		graph.SetInput(mapping, "Scale", cinema::Vector(repeats.x, repeats.y, 1.0));
		graph.Link(NodeOutput { coordinates, "UV"_s }, mapping, "Vector") iferr_return;
		graph.Link(NodeOutput { mapping, "Vector"_s }, texture, "Vector") iferr_return;
	}
	NodeOutput value { texture, "Alpha"_s };
	if (mode == 0)
	{
		// The intensity: the mean of the three channels.
		const maxon::Int mean = graph.Add("ShaderNodeVectorMath") iferr_return;
		graph.SetProperty(mean, "operation", "DOT_PRODUCT"_s);
		graph.Link(NodeOutput { texture, "Color"_s }, mean, "Vector") iferr_return;
		graph.SetInput(mean, "Vector_001", cinema::Vector(1.0 / 3.0));
		value = NodeOutput { mean, "Value"_s };
	}
	// 1 where the value reaches the threshold: not (value < threshold).
	const maxon::Int below = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(below, "operation", "LESS_THAN"_s);
	graph.Link(value, below, "Value") iferr_return;
	graph.SetInput(below, "Value_001", threshold);
	const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
	const maxon::Int mix = graph.Add("ShaderNodeMixShader") iferr_return;
	graph.Link(NodeOutput { below, "Value"_s }, mix, "Fac") iferr_return;
	graph.Link(shown, mix, 1) iferr_return;
	graph.Link(NodeOutput { transparent, "BSDF"_s }, mix, 2) iferr_return;
	surface = NodeOutput { mix, "Shader"_s };
	return true;
}

// A material node: the Standard Material, OpenPBR, Incandescent, Sprite, or a Store To AOV node
// in front of one of them. False for any other.
static maxon::Result<maxon::Bool> BuildSurfaceNode(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, const maxon::GraphNode& start,
	NodeOutput& surface, maxon::Int depth)
{
	iferr_scope;
	maxon::GraphNode node = start;
	if (depth > 8)
		return false;
	for (maxon::Int hop = 0; hop < 16 && node.IsValid(); ++hop)
	{
		const maxon::String name = RedshiftNodes::ShortName(node) iferr_return;
		if (name != "storecolortoaov"_s && name != "storescalartoaov"_s && name != "storeintegertoaov"_s)
			break;
		// Writes values into AOVs and passes its beauty input on; custom AOVs are not made.
		exporter.Warn(FormatString("Material \"@\": the \"@\" node's AOVs are not written (custom AOVs are not translated); its beauty input renders.",
			material->GetName(), name));
		maxon::Id port;
		port.Init(maxon::String(RS_NODE) + name + ".beauty_input"_s) iferr_return;
		node = WiredNode(node, port) iferr_return;
	}
	if (!node.IsValid())
		return false;
	const maxon::String name = RedshiftNodes::ShortName(node) iferr_return;
	if (name == "standardmaterial"_s)
		return BuildStandardSurface(exporter, graph, material, node, STANDARD_PORTS, surface);
	if (name == "openpbrmaterial"_s)
		return BuildStandardSurface(exporter, graph, material, node, OPENPBR_PORTS, surface);
	if (name == "incandescent"_s)
		return BuildIncandescent(exporter, graph, material, node, surface);
	if (name == "sprite"_s)
		return BuildSprite(exporter, graph, material, node, surface, depth);
	return false;
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
	const maxon::GraphNode node = OutputSurface(model) iferr_return;
	if (!node.IsValid())
		return false;
	return BuildSurfaceNode(exporter, graph, material, node, surface, 0);
}

} // namespace cyclesrender
