// SPDX-License-Identifier: Apache-2.0
// Redshift's shading nodes as Blender nodes (redshift_nodes.h). Reading order: finding what is
// wired into a port, the tables of nodes that become one Blender node, then Translate, which
// handles the constants, the basic math, mixing, colour math, makers and splitters itself and hands
// the others to the groups below; Color Correct and
// the Texture node; then the terms (expressions of Blender nodes) the groups build with, and the
// groups: more math, colour, utility, textures. Every formula was measured against Redshift's
// render (docs/reference/c4d-sdk.md, "Redshift Standard Material").

#include "c4d.h"
#include "c4d_basematerial.h"
#include "drendersettings.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/url.h"

#include "../sdk_compat.h"
#include "../scene/coords.h"
#include "cycles_node_material.h"
#include "material_export.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

namespace cyclesrender
{

// Blender's Wireframe size, in pixels, for a Redshift WireFrame thickness of 1 (measured).
static const maxon::Float64 WIRE_PIXELS = 1.0;

// A chain this deep is a loop or more than any material needs.
static const maxon::Int MAX_DEPTH = 64;

// Redshift's scalar math nodes that are one Blender Math node: its operation and the Redshift
// inputs that become the Math node's first and second input (nullptr: none, the default stays).
struct MathNode
{
	const maxon::Char* name;
	const maxon::Char* operation;
	const maxon::Char* first;
	const maxon::Char* second;
};

static const MathNode MATH[] =
{
	{ "rsmathadd", "ADD", "input1", "input2" },
	{ "rsmathsub", "SUBTRACT", "input1", "input2" },
	{ "rsmathmul", "MULTIPLY", "input1", "input2" },
	{ "rsmathdiv", "DIVIDE", "input1", "input2" },
	{ "rsmathmin", "MINIMUM", "input1", "input2" },
	{ "rsmathmax", "MAXIMUM", "input1", "input2" },
	{ "rsmathpow", "POWER", "base", "exponent" },
	{ "rsmathmod", "MODULO", "input", "divisor" },
	{ "rsmathabs", "ABSOLUTE", "input", nullptr },
	{ "rsmathfloor", "FLOOR", "input", nullptr },
	{ "rsmathfrac", "FRACT", "input", nullptr },
	{ "rsmathsqrt", "SQRT", "input", nullptr },
	{ "rsmathexp", "EXPONENT", "input", nullptr },
	{ "rsmathsign", "SIGN", "input", nullptr },
};

// Redshift's vector math nodes that are one Blender Vector Math node, and the output it gives.
struct VectorNode
{
	const maxon::Char* name;
	const maxon::Char* operation;
	maxon::Bool binary;
	const maxon::Char* output;
};

static const VectorNode VECTOR_MATH[] =
{
	{ "rsmathaddvector", "ADD", true, "Vector" },
	{ "rsmathsubvector", "SUBTRACT", true, "Vector" },
	{ "rsmathmulvector", "MULTIPLY", true, "Vector" },
	{ "rsmathdivvector", "DIVIDE", true, "Vector" },
	{ "rsmathminvector", "MINIMUM", true, "Vector" },
	{ "rsmathmaxvector", "MAXIMUM", true, "Vector" },
	{ "rsmathcrossvector", "CROSS_PRODUCT", true, "Vector" },
	{ "rsmathdotvector", "DOT_PRODUCT", true, "Value" },
	{ "rsmathabsvector", "ABSOLUTE", false, "Vector" },
	{ "rsmathlengthvector", "LENGTH", false, "Value" },
	{ "rsmathnormalizevector", "NORMALIZE", false, "Vector" },
};

// A value set on a port: a number or a colour or vector.
struct PortValue
{
	maxon::Bool found = false;
	maxon::Bool isVector = false;
	maxon::Float64 number = 0.0;
	cinema::Vector vector;
	maxon::Float64 alpha = 1.0;
};

static PortValue ReadValue(const maxon::GraphNode& port)
{
	PortValue value;
	if (!port.IsValid())
		return value;
	value.found = true;
	ifnoerr (const maxon::Opt<const maxon::Float&> number = port.GetEffectivePortValue<maxon::Float>())
	{
		if (number)
		{
			value.number = *number;
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Float32&> number32 = port.GetEffectivePortValue<maxon::Float32>())
	{
		if (number32)
		{
			value.number = *number32;
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Int&> whole = port.GetEffectivePortValue<maxon::Int>())
	{
		if (whole)
		{
			value.number = maxon::Float64(*whole);
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Bool&> flag = port.GetEffectivePortValue<maxon::Bool>())
	{
		if (flag)
		{
			value.number = *flag ? 1.0 : 0.0;
			return value;
		}
	}
	value.isVector = true;
	ifnoerr (const maxon::Opt<const maxon::Color&> rgb = port.GetEffectivePortValue<maxon::Color>())
	{
		if (rgb)
		{
			value.vector = cinema::Vector(rgb->r, rgb->g, rgb->b);
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::ColorA&> rgba = port.GetEffectivePortValue<maxon::ColorA>())
	{
		if (rgba)
		{
			value.vector = cinema::Vector(rgba->r, rgba->g, rgba->b);
			value.alpha = rgba->a;
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Vector&> vector = port.GetEffectivePortValue<maxon::Vector>())
	{
		if (vector)
		{
			value.vector = *vector;
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Vector32&> vector32 = port.GetEffectivePortValue<maxon::Vector32>())
	{
		if (vector32)
		{
			value.vector = cinema::Vector(vector32->x, vector32->y, vector32->z);
			return value;
		}
	}
	ifnoerr (const maxon::Opt<const maxon::Vector2d&> vector2 = port.GetEffectivePortValue<maxon::Vector2d>())
	{
		if (vector2)
		{
			value.vector = cinema::Vector(vector2->x, vector2->y, 0.0);
			return value;
		}
	}
	value.found = false;
	return value;
}

// An id put together at run time (maxon::Id has no constructor from a String).
static maxon::Result<maxon::Id> MakeId(const maxon::String& text)
{
	iferr_scope;
	maxon::Id id;
	id.Init(text) iferr_return;
	return id;
}

// The input port of a Redshift node by its short name ("input1").
static maxon::Result<maxon::GraphNode> InputPort(const maxon::GraphNode& node, const maxon::String& asset, const maxon::Char* name)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	const maxon::Id id = MakeId(asset + "."_s + maxon::String(name)) iferr_return;
	return inputs.FindChild(id);
}

static maxon::Result<maxon::Id> AssetOf(const maxon::GraphNode& node)
{
	iferr_scope;
	const maxon::IdAndVersion asset = OptOrDefault(node.GetValue<decltype(maxon::nodes::AssetId)>()) iferr_return;
	return asset.Get<0>();
}

maxon::Result<maxon::String> RedshiftNodes::ShortName(const maxon::GraphNode& node)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(node) iferr_return;
	maxon::String name = asset.ToString();
	const maxon::String prefix = maxon::String(RS_NODE);
	if (name.StartsWith(prefix))
		name = name.GetPart(prefix.GetLength(), name.GetLength() - prefix.GetLength());
	return name;
}

maxon::Result<RedshiftNodes::Source> RedshiftNodes::Find(const maxon::GraphNode& port)
{
	iferr_scope;
	Source source;
	if (!port.IsValid())
		return source;
	maxon::GraphNode output;
	port.GetConnections(maxon::PORT_DIR::INPUT, [&output](const maxon::GraphConnection& connection) -> maxon::Result<maxon::Bool>
		{
			output = connection.first;
			return false;
		}) iferr_return;
	if (!output.IsValid())
		return source;
	// Through Wire Rerouters and across the borders of groups (0.3.0, F13 W19; F12's mechanism for Cycles graphs). A wire
	// that ends at a group's input holding a value brings nothing: the input it leads to has that value as its own.
	output = ThroughBorders(output) iferr_return;
	if (!output.IsValid())
		return source;
	source.node = output.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
	// The output's id is "<node asset>.<output>": keep the last part.
	const maxon::String id = output.GetId().ToString();
	const maxon::Int dot = id.FindLastIndex('.');
	source.output = dot >= 0 ? id.GetPart(dot + 1, id.GetLength() - dot - 1) : id;
	return source;
}

maxon::Result<maxon::GraphNode> RedshiftNodes::SourceNode(const maxon::GraphNode& port)
{
	iferr_scope;
	const Source source = Find(port) iferr_return;
	return source.node;
}

void RedshiftNodes::Warn(const maxon::String& text) const
{
	_exporter.Warn(FormatString("Material \"@\": @", _material->GetName(), text));
}

void RedshiftNodes::NotTranslated(const maxon::GraphNode& node, const maxon::Char* what) const
{
	maxon::String name;
	iferr (name = ShortName(node))
		name = maxon::String();
	Warn(FormatString("the \"@\" node plugged into @ is not translated; the value set on the input is used.", name, maxon::String(what)));
}

maxon::Result<NodeOutput> RedshiftNodes::Wired(const maxon::GraphNode& port, maxon::Bool colorData, const maxon::Char* what)
{
	iferr_scope;
	const Source source = Find(port) iferr_return;
	if (!source.node.IsValid())
		return NodeOutput();
	return Translate(source, colorData, what, 0);
}

maxon::Result<void> RedshiftNodes::Feed(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Int node, const maxon::Char* socket,
	maxon::Bool colorData, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode port = InputPort(rsNode, asset.ToString(), name) iferr_return;
	const PortValue value = ReadValue(port);
	if (value.found)
	{
		if (value.isVector)
			_graph.SetInput(node, socket, value.vector);
		else
			_graph.SetInput(node, socket, value.number);
	}
	const Source source = Find(port) iferr_return;
	if (!source.node.IsValid())
		return maxon::OK;
	const NodeOutput wired = Translate(source, colorData, what, depth + 1) iferr_return;
	if (wired.IsValid())
	{
		_graph.Link(wired, node, socket) iferr_return;
	}
	return maxon::OK;
}

maxon::Result<NodeOutput> RedshiftNodes::Translate(const Source& source, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	if (depth > MAX_DEPTH)
	{
		Warn(FormatString("the nodes plugged into @ are nested too deeply (or in a loop); the value set on the input is used.", maxon::String(what)));
		return NodeOutput();
	}
	const maxon::String name = ShortName(source.node) iferr_return;
	const maxon::GraphNode& rs = source.node;

	if (name == "texturesampler"_s)
		return TextureSampler(rs, colorData, what);

	// Constants.
	if (name == "rsscalarconstant"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeValue") iferr_return;
		const maxon::GraphNode port = InputPort(rs, RS_NODE "rsscalarconstant"_s, "val") iferr_return;
		const PortValue value = ReadValue(port);
		_graph.SetOutputJson(node, "Value", FormatString("@", value.number));
		return NodeOutput { node, "Value"_s };
	}
	if (name == "rscolorconstant"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeRGB") iferr_return;
		const maxon::GraphNode port = InputPort(rs, RS_NODE "rscolorconstant"_s, "color") iferr_return;
		const PortValue value = ReadValue(port);
		_graph.SetOutputJson(node, "Color", FormatString("[@, @, @, 1.0]", value.vector.x, value.vector.y, value.vector.z));
		return NodeOutput { node, "Color"_s };
	}

	// Scalar math.
	for (const MathNode& math : MATH)
	{
		if (name != maxon::String(math.name))
			continue;
		const maxon::Int node = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(node, "operation", maxon::String(math.operation));
		Feed(rs, math.first, node, "Value", false, what, depth) iferr_return;
		if (math.second)
		{
			Feed(rs, math.second, node, "Value_001", false, what, depth) iferr_return;
		}
		return NodeOutput { node, "Value"_s };
	}
	if (name == "rsmathinv"_s || name == "rsmathrcp"_s || name == "rsmathneg"_s || name == "rsmathln"_s || name == "rsmathsaturate"_s)
	{
		// 1 - x, 1 / x, x * -1, log(x) to the base e, x clamped to 0..1.
		const maxon::Int node = _graph.Add("ShaderNodeMath") iferr_return;
		if (name == "rsmathinv"_s || name == "rsmathrcp"_s)
		{
			_graph.SetProperty(node, "operation", name == "rsmathinv"_s ? "SUBTRACT"_s : "DIVIDE"_s);
			_graph.SetInput(node, "Value", 1.0);
			Feed(rs, "input", node, "Value_001", false, what, depth) iferr_return;
		}
		else
		{
			_graph.SetProperty(node, "operation", name == "rsmathneg"_s ? "MULTIPLY"_s : name == "rsmathln"_s ? "LOGARITHM"_s : "ADD"_s);
			Feed(rs, "input", node, "Value", false, what, depth) iferr_return;
			_graph.SetInput(node, "Value_001", name == "rsmathneg"_s ? -1.0 : name == "rsmathln"_s ? maxon::Exp(1.0) : 0.0);
			if (name == "rsmathsaturate"_s)
				_graph.SetProperty(node, "use_clamp", true);
		}
		return NodeOutput { node, "Value"_s };
	}
	if (name == "rsmathsin"_s || name == "rsmathcos"_s || name == "rsmathtan"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeMath") iferr_return;
		_graph.SetProperty(node, "operation", name == "rsmathsin"_s ? "SINE"_s : name == "rsmathcos"_s ? "COSINE"_s : "TANGENT"_s);
		const maxon::GraphNode port = InputPort(rs, maxon::String(RS_NODE) + name, "isradians") iferr_return;
		const PortValue radians = ReadValue(port);
		if (radians.found && radians.number == 0.0)
		{
			// The input is in degrees.
			const maxon::Int convert = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(convert, "operation", "RADIANS"_s);
			Feed(rs, "input", convert, "Value", false, what, depth) iferr_return;
			_graph.Link(NodeOutput { convert, "Value"_s }, node, "Value") iferr_return;
		}
		else
		{
			Feed(rs, "input", node, "Value", false, what, depth) iferr_return;
		}
		return NodeOutput { node, "Value"_s };
	}
	if (name == "rsmathrange"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeMapRange") iferr_return;
		Feed(rs, "input", node, "Value", false, what, depth) iferr_return;
		Feed(rs, "old_min", node, "From Min", false, what, depth) iferr_return;
		Feed(rs, "old_max", node, "From Max", false, what, depth) iferr_return;
		Feed(rs, "new_min", node, "To Min", false, what, depth) iferr_return;
		Feed(rs, "new_max", node, "To Max", false, what, depth) iferr_return;
		const maxon::GraphNode port = InputPort(rs, RS_NODE "rsmathrange"_s, "clamp") iferr_return;
		const PortValue clamp = ReadValue(port);
		_graph.SetProperty(node, "clamp", !clamp.found || clamp.number != 0.0);
		return NodeOutput { node, "Result"_s };
	}

	// Mixing: a number, a vector, a colour.
	if (name == "rsmathmix"_s || name == "rsmathmixvector"_s || name == "rscolormix"_s)
	{
		const maxon::Bool color = name == "rscolormix"_s;
		const maxon::Bool vector = name == "rsmathmixvector"_s;
		const maxon::Int node = _graph.Add("ShaderNodeMix") iferr_return;
		_graph.SetProperty(node, "data_type", color ? "RGBA"_s : vector ? "VECTOR"_s : "FLOAT"_s);
		_graph.SetProperty(node, "blend_type", "MIX"_s);
		// Redshift does not clamp the factor (measured: 1.5 mixes beyond the second input).
		_graph.SetProperty(node, "clamp_factor", false);
		const maxon::Char* const a = color ? "A_Color" : vector ? "A_Vector" : "A_Float";
		const maxon::Char* const b = color ? "B_Color" : vector ? "B_Vector" : "B_Float";
		Feed(rs, "mixamount", node, "Factor_Float", false, what, depth) iferr_return;
		Feed(rs, "input1", node, a, colorData, what, depth) iferr_return;
		Feed(rs, "input2", node, b, colorData, what, depth) iferr_return;
		return NodeOutput { node, color ? "Result_Color"_s : vector ? "Result_Vector"_s : "Result_Float"_s };
	}

	// Vector math.
	for (const VectorNode& math : VECTOR_MATH)
	{
		if (name != maxon::String(math.name))
			continue;
		const maxon::Int node = _graph.Add("ShaderNodeVectorMath") iferr_return;
		_graph.SetProperty(node, "operation", maxon::String(math.operation));
		Feed(rs, math.binary ? "input1" : "input", node, "Vector", false, what, depth) iferr_return;
		if (math.binary)
		{
			Feed(rs, "input2", node, "Vector_001", false, what, depth) iferr_return;
		}
		return NodeOutput { node, maxon::String(math.output) };
	}

	// Colour math.
	if (name == "rsmathinvcolor"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeInvert") iferr_return;
		_graph.SetInput(node, "Fac", 1.0);
		Feed(rs, "input", node, "Color", colorData, what, depth) iferr_return;
		return NodeOutput { node, "Color"_s };
	}
	if (name == "rsmathsubcolor"_s || name == "rsmathsaturatecolor"_s)
	{
		// a - b; or a clamped to 0..1 (a mix with nothing that clamps its result).
		const maxon::Bool subtract = name == "rsmathsubcolor"_s;
		const maxon::Int node = _graph.Add("ShaderNodeMix") iferr_return;
		_graph.SetProperty(node, "data_type", "RGBA"_s);
		_graph.SetProperty(node, "blend_type", subtract ? "SUBTRACT"_s : "MIX"_s);
		_graph.SetInput(node, "Factor_Float", subtract ? 1.0 : 0.0);
		Feed(rs, subtract ? "input1" : "input", node, "A_Color", colorData, what, depth) iferr_return;
		if (subtract)
		{
			Feed(rs, "input2", node, "B_Color", colorData, what, depth) iferr_return;
		}
		else
		{
			_graph.SetProperty(node, "clamp_result", true);
		}
		return NodeOutput { node, "Result_Color"_s };
	}

	// Making and splitting colours and vectors.
	if (name == "rscolormaker"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeCombineColor") iferr_return;
		Feed(rs, "red", node, "Red", false, what, depth) iferr_return;
		Feed(rs, "green", node, "Green", false, what, depth) iferr_return;
		Feed(rs, "blue", node, "Blue", false, what, depth) iferr_return;
		return NodeOutput { node, "Color"_s };
	}
	if (name == "rsvectormaker"_s)
	{
		const maxon::Int node = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
		Feed(rs, "x", node, "X", false, what, depth) iferr_return;
		Feed(rs, "y", node, "Y", false, what, depth) iferr_return;
		Feed(rs, "z", node, "Z", false, what, depth) iferr_return;
		return NodeOutput { node, "Vector"_s };
	}
	if (name == "rscolorsplitter"_s || name == "rsvectortoscalars"_s)
	{
		const maxon::Bool color = name == "rscolorsplitter"_s;
		if (source.output == "outa"_s)
		{
			// Blender's colours carry no alpha along a wire: it is followed only to a node that reads one
			// from the scene (a Color User Data, a Vertex or Point Attribute node), else the colour's own
			// value on the input counts.
			const maxon::GraphNode input = InputPort(rs, RS_NODE "rscolorsplitter"_s, "input") iferr_return;
			const Source from = Find(input) iferr_return;
			if (!from.node.IsValid())
			{
				const PortValue value = ReadValue(input);
				return Output(Term::Number(value.found ? value.alpha : 1.0), true);
			}
			maxon::Bool known = false;
			const Term alpha = AlphaOf(from, what, known) iferr_return;
			if (!known)
			{
				Warn(FormatString("the alpha output of a Color Splitter plugged into @ is translated only for a colour that comes straight from a "
					"Color User Data, Vertex Attribute or Point Attribute node (Blender's colours have no alpha); the value set on the input is used.",
					maxon::String(what)));
				return NodeOutput();
			}
			return Output(alpha, true);
		}
		const maxon::Int node = _graph.Add(color ? "ShaderNodeSeparateColor" : "ShaderNodeSeparateXYZ") iferr_return;
		Feed(rs, "input", node, color ? "Color" : "Vector", colorData, what, depth) iferr_return;
		maxon::String socket;
		if (source.output == "outr"_s || source.output == "outx"_s)
			socket = color ? "Red"_s : "X"_s;
		else if (source.output == "outg"_s || source.output == "outy"_s)
			socket = color ? "Green"_s : "Y"_s;
		else
			socket = color ? "Blue"_s : "Z"_s;
		return NodeOutput { node, socket };
	}
	if (name == "rscolor2hsv"_s || name == "rshsv2color"_s)
	{
		// Hue, saturation and value in the colour's three channels, each 0 to 1.
		const maxon::Bool toHsv = name == "rscolor2hsv"_s;
		const maxon::Int split = _graph.Add("ShaderNodeSeparateColor") iferr_return;
		_graph.SetProperty(split, "mode", toHsv ? "HSV"_s : "RGB"_s);
		Feed(rs, "input", split, "Color", colorData, what, depth) iferr_return;
		const maxon::Int join = _graph.Add("ShaderNodeCombineColor") iferr_return;
		_graph.SetProperty(join, "mode", toHsv ? "RGB"_s : "HSV"_s);
		_graph.Link(NodeOutput { split, "Red"_s }, join, "Red") iferr_return;
		_graph.Link(NodeOutput { split, "Green"_s }, join, "Green") iferr_return;
		_graph.Link(NodeOutput { split, "Blue"_s }, join, "Blue") iferr_return;
		return NodeOutput { join, "Color"_s };
	}

	// Colour correction: gamma, then contrast, then hue and saturation, then level (measured
	// against Redshift: docs/reference/c4d-sdk.md, "Redshift nodes").
	if (name == "rscolorcorrection"_s)
	{
		return ColorCorrection(rs, colorData, what, depth);
	}

	// The groups: more math, colour, utility and texture nodes.
	maxon::Bool handled = false;
	NodeOutput result = MoreMath(source, name, colorData, what, depth, handled) iferr_return;
	if (!handled)
	{
		result = ColorNodes(source, name, colorData, what, depth, handled) iferr_return;
	}
	if (!handled)
	{
		result = UtilityNodes(source, name, colorData, what, depth, handled) iferr_return;
	}
	if (!handled)
	{
		result = TextureNodes(source, name, colorData, what, depth, handled) iferr_return;
	}
	if (!handled)
	{
		result = SceneDataNodes(source, name, what, handled) iferr_return;
	}
	if (!handled)
	{
		result = LookAlikes(source, name, colorData, what, depth, handled) iferr_return;
	}
	if (handled)
		return result;

	NotTranslated(rs, what);
	return NodeOutput();
}

//----------------------------------------------------------------------------------------
// Redshift's Color Correct, measured against Redshift's own render of a constant colour
// (docs/reference/c4d-sdk.md, "Redshift nodes"): gamma g gives x^(1/g); contrast c is Schlick's
// gain with a = 1 - c around 0.5 (x / (k (1 - 2x) + 1) below 0.5 and 1 - (1 - x) / (k (2x - 1) + 1)
// above, k = 1/a - 2; 0.5 changes nothing); hue is a turn of the HSV hue in degrees; saturation
// multiplies the HSV saturation; level multiplies the colour; in this order. Written with Blender's
// nodes as one expression per channel: (x + k max(2x - 1, 0)) / (1 + k |2x - 1|). Blender clamps
// the saturation at 1 where Redshift does not. Gamma, hue, saturation and level may be wired (0.3.0: a
// hue and a level that follow a particle's alpha); the contrast counts with its value, and what is
// wired into it is named and left out.
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::ColorCorrection(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	auto setting = [&](const maxon::Char* id, maxon::Float64 fallback) -> maxon::Result<maxon::Float64>
	{
		iferr_scope;
		const maxon::GraphNode port = InputPort(node, RS_NODE "rscolorcorrection"_s, id) iferr_return;
		const Source wired = Find(port) iferr_return;
		if (wired.node.IsValid())
			Warn(FormatString("the \"@\" of a Color Correct node plugged into @ is wired; only its value is translated.", maxon::String(id), maxon::String(what)));
		const PortValue value = ReadValue(port);
		return value.found ? value.number : fallback;
	};
	// A setting that may be wired: its value, or what is wired into it.
	auto wirable = [&](const maxon::Char* id, maxon::Float64 fallback) -> maxon::Result<Term>
	{
		iferr_scope;
		const maxon::GraphNode port = InputPort(node, RS_NODE "rscolorcorrection"_s, id) iferr_return;
		const Term term = Input(node, id, false, what, depth) iferr_return;
		if (!term.IsConstant() || ReadValue(port).found)
			return term;
		return Term::Number(fallback);
	};
	const Term gamma = wirable("gamma", 1.0) iferr_return;
	const maxon::Float64 contrast = setting("contrast", 0.5) iferr_return;
	const Term hue = wirable("hue", 0.0) iferr_return;
	const Term saturation = wirable("saturation", 1.0) iferr_return;
	const Term level = wirable("level", 1.0) iferr_return;

	// The input, through a Gamma node that may change nothing (it holds the input).
	const maxon::Int gammaNode = _graph.Add("ShaderNodeGamma") iferr_return;
	if (gamma.IsConstant())
	{
		_graph.SetInput(gammaNode, "Gamma", gamma.value.x > 0.0 ? 1.0 / gamma.value.x : 1.0);
	}
	else
	{
		const Term inverse = Math("DIVIDE", Term::Number(1.0), gamma) iferr_return;
		Set(gammaNode, "Gamma", inverse, true) iferr_return;
	}
	Feed(node, "input", gammaNode, "Color", colorData, what, depth) iferr_return;
	NodeOutput color { gammaNode, "Color"_s };

	if (contrast != 0.5)
	{
		const maxon::Float64 a = maxon::ClampValue(1.0 - contrast, 1e-6, 1.0);
		const cinema::Vector k(1.0 / a - 2.0);
		auto vector = [&](const maxon::Char* operation) -> maxon::Result<maxon::Int>
		{
			iferr_scope;
			const maxon::Int math = _graph.Add("ShaderNodeVectorMath") iferr_return;
			_graph.SetProperty(math, "operation", maxon::String(operation));
			return math;
		};
		const maxon::Int w = vector("MULTIPLY_ADD") iferr_return;        // 2x - 1
		_graph.Link(color, w, "Vector") iferr_return;
		_graph.SetInput(w, "Vector_001", cinema::Vector(2.0));
		_graph.SetInput(w, "Vector_002", cinema::Vector(-1.0));
		const maxon::Int upper = vector("MAXIMUM") iferr_return;         // max(2x - 1, 0)
		_graph.Link(NodeOutput { w, "Vector"_s }, upper, "Vector") iferr_return;
		_graph.SetInput(upper, "Vector_001", cinema::Vector(0.0));
		const maxon::Int distance = vector("ABSOLUTE") iferr_return;     // |2x - 1|
		_graph.Link(NodeOutput { w, "Vector"_s }, distance, "Vector") iferr_return;
		const maxon::Int numerator = vector("MULTIPLY_ADD") iferr_return;  // x + k max(2x - 1, 0)
		_graph.Link(NodeOutput { upper, "Vector"_s }, numerator, "Vector") iferr_return;
		_graph.SetInput(numerator, "Vector_001", k);
		_graph.Link(color, numerator, "Vector_002") iferr_return;
		const maxon::Int denominator = vector("MULTIPLY_ADD") iferr_return;  // 1 + k |2x - 1|
		_graph.Link(NodeOutput { distance, "Vector"_s }, denominator, "Vector") iferr_return;
		_graph.SetInput(denominator, "Vector_001", k);
		_graph.SetInput(denominator, "Vector_002", cinema::Vector(1.0));
		const maxon::Int divide = vector("DIVIDE") iferr_return;
		_graph.Link(NodeOutput { numerator, "Vector"_s }, divide, "Vector") iferr_return;
		_graph.Link(NodeOutput { denominator, "Vector"_s }, divide, "Vector_001") iferr_return;
		color = NodeOutput { divide, "Vector"_s };
	}
	if (!hue.IsConstant() || !saturation.IsConstant() || !level.IsConstant() || hue.value.x != 0.0 || saturation.value.x != 1.0 || level.value.x != 1.0)
	{
		if (!saturation.IsConstant() || saturation.value.x > 1.0)
			Warn(FormatString("a Color Correct node plugged into @ @, which Blender clamps at 1.", maxon::String(what),
				saturation.IsConstant() ? "raises the saturation above 1"_s : "has a wired saturation: where that is above 1"_s));
		const maxon::Int hsv = _graph.Add("ShaderNodeHueSaturation") iferr_return;
		if (hue.IsConstant())
		{
			_graph.SetInput(hsv, "Hue", 0.5 + hue.value.x / 360.0 - maxon::Floor(0.5 + hue.value.x / 360.0));
		}
		else
		{
			// Degrees as a share of the turn, around the node's 0.5 for "no turn"; the node wraps it.
			const Term turn = Math("MULTIPLY_ADD", hue, Term::Number(1.0 / 360.0), Term::Number(0.5)) iferr_return;
			Set(hsv, "Hue", turn, true) iferr_return;
		}
		Set(hsv, "Saturation", saturation, true) iferr_return;
		Set(hsv, "Value", level, true) iferr_return;
		_graph.SetInput(hsv, "Fac", 1.0);
		_graph.Link(color, hsv, "Color") iferr_return;
		color = NodeOutput { hsv, "Color"_s };
	}
	return color;
}

// The file named by a node's "tex0" bundle, as the id of an `image` item; empty with a warning
// naming @p label when there is none.
maxon::Result<maxon::String> RedshiftNodes::FileImage(const maxon::GraphNode& node, const maxon::String& asset, maxon::Bool colorData, const maxon::Char* what,
	const maxon::Char* label)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	const maxon::Id bundleId = MakeId(maxon::String(RS_NODE) + asset + ".tex0"_s) iferr_return;
	const maxon::GraphNode bundle = inputs.FindChild(bundleId) iferr_return;
	maxon::GraphNode pathPort;
	if (bundle.IsValid())
	{
		pathPort = bundle.FindChild(maxon::Id("path")) iferr_return;
	}
	// The file as a path Blender can open (MaterialExporter::ResolveImageUrl): an image of the Asset Browser
	// (a URL of the scheme "asset") is copied out of its database, a relative path is joined to the document's
	// folder, and a name is looked for as Cinema 4D looks for a bitmap's.
	maxon::String path;
	maxon::String problem;
	maxon::Bool named = false;
	if (pathPort.IsValid())
	{
		ifnoerr (const maxon::Opt<const maxon::Url&> url = pathPort.GetEffectivePortValue<maxon::Url>())
		{
			if (url && url->IsPopulated())
			{
				named = true;
				path = _exporter.ResolveImageUrl(*url, problem) iferr_return;
			}
		}
		if (!named)
		{
			ifnoerr (const maxon::Opt<const maxon::String&> text = pathPort.GetEffectivePortValue<maxon::String>())
			{
				if (text && text->IsPopulated())
				{
					named = true;
					path = _exporter.ResolveImagePath(*text, problem) iferr_return;
				}
			}
		}
	}
	if (problem.IsPopulated())
		Warn(FormatString("the @ node plugged into @: @", maxon::String(label), maxon::String(what), problem));
	if (path.IsEmpty())
	{
		if (!named)
			Warn(FormatString("the @ node plugged into @ names no file.", maxon::String(label), maxon::String(what)));
		return maxon::String();
	}
	return _exporter.GetFileImageId(path, colorData);
}

// A pair of numbers on a port of the Texture node (its scale and offset).
static maxon::Vector2d PairValue(const maxon::GraphNode& port, const maxon::Vector2d& fallback)
{
	if (!port.IsValid())
		return fallback;
	ifnoerr (const maxon::Opt<const maxon::Vector2d&> value = port.GetEffectivePortValue<maxon::Vector2d>())
	{
		if (value)
			return *value;
	}
	return fallback;
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::MaterialUv()
{
	iferr_scope;
	const TextureMapping& mapping = _exporter.GetCurrentMapping();
	NodeOutput uv;
	if (!mapping.IsPlain())
	{
		const ProjectedUv projected = _exporter.GetCurrentProjection(_graph, _material->GetName()) iferr_return;
		// A cubic projection gives a point for Blender's box projection of images, no UV pair: the UV map stands in.
		if (!projected.box)
		{
			uv = TileTextureTag(_graph, projected.uv, mapping) iferr_return;
			// Repeated: a Ramp shows as many copies as the tag has tiles (measured).
			if (uv.IsValid() && mapping.tile)
			{
				const Term repeated = VMath("FRACTION", Term::Of(uv)) iferr_return;
				uv = repeated.out;
			}
		}
	}
	if (!uv.IsValid())
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		uv = NodeOutput { coordinates, "UV"_s };
	}
	return Term::Of(uv);
}

maxon::Result<NodeOutput> RedshiftNodes::TextureSampler(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what, const Term* vector)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;

	// The file's colour space is the node's own, whatever input the node is plugged into (measured, 0.3.0, F13 W18:
	// a grey of 128 renders 0.502 with "RAW" and 0.216 with "sRGB" or no space set, also into a Color Maker's numbers).
	maxon::String space;
	const maxon::GraphNode bundle = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.tex0")) iferr_return;
	if (bundle.IsValid())
	{
		const maxon::GraphNode spacePort = bundle.FindChild(maxon::Id("colorspace")) iferr_return;
		if (spacePort.IsValid())
		{
			ifnoerr (const maxon::Opt<const maxon::String&> text = spacePort.GetEffectivePortValue<maxon::String>())
			{
				if (text)
					space = *text;
			}
		}
	}
	maxon::Bool asColor = colorData;
	if (space == "RS_INPUT_COLORSPACE_RAW"_s)
		asColor = false;
	else if (space.IsEmpty() || space == "RS_INPUT_COLORSPACE_SRGB"_s)
		asColor = true;
	else
		Warn(FormatString("the colour space \"@\" of a Texture node plugged into @ is not translated; the file is read as @.", space, maxon::String(what),
			colorData ? "colours"_s : "data"_s));
	const maxon::String image = FileImage(node, "texturesampler"_s, asColor, what, "Texture") iferr_return;
	if (image.IsEmpty())
		return NodeOutput();
	const maxon::Int texture = _graph.Add("ShaderNodeTexImage") iferr_return;
	_graph.SetProperty(texture, "image", image);

	// Where it is looked up (measured, F13 W18): the UV map, or the coordinates a projection node gives, or the texture tag's
	// projection; then the node's own rotation about the middle (degrees, counter-clockwise with V up), offset (V up) and
	// scale; then the texture tag's offset and length, as Cinema 4D's u' = (u - offset) / length.
	const maxon::GraphNode scalePort = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.scale")) iferr_return;
	const maxon::GraphNode offsetPort = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.offset")) iferr_return;
	const maxon::Vector2d scale = PairValue(scalePort, maxon::Vector2d(1.0));
	const maxon::Vector2d offset = PairValue(offsetPort, maxon::Vector2d(0.0));
	const maxon::Float64 rotate = Setting(node, "rotate", 0.0, what, " Texture") iferr_return;
	const TextureMapping& mapping = vector ? TextureMapping() : _exporter.GetCurrentMapping();
	ProjectedUv projected;
	if (!mapping.IsPlain())
	{
		projected = _exporter.GetCurrentProjection(_graph, _material->GetName()) iferr_return;
	}
	Term coordinates;
	maxon::Bool placed = false;
	if (vector)
	{
		coordinates = *vector;
		placed = true;
	}
	else if (projected.uv.IsValid())
	{
		coordinates = Term::Of(projected.uv);
		placed = true;
	}
	const maxon::Bool own = scale != maxon::Vector2d(1.0) || offset != maxon::Vector2d(0.0) || rotate != 0.0;
	if (own && projected.box)
	{
		Warn(FormatString("the Texture node plugged into @ has a scale, offset or rotation of its own, which is not translated on a cubic projection.",
			maxon::String(what)));
	}
	else if (own)
	{
		if (!placed)
		{
			const maxon::Int uv = _graph.Add("ShaderNodeTexCoord") iferr_return;
			coordinates = Term::Of(NodeOutput { uv, "UV"_s });
			placed = true;
		}
		if (rotate != 0.0 || offset != maxon::Vector2d(0.0))
		{
			// Blender's Mapping node: R v + L, with L = c - R c + offset turning about the middle c of the UV square.
			const maxon::Float64 angle = rotate * maxon::PI / 180.0;
			const maxon::Float64 c = maxon::Cos(angle);
			const maxon::Float64 s = maxon::Sin(angle);
			const maxon::Int turn = _graph.Add("ShaderNodeMapping") iferr_return;
			_graph.SetInput(turn, "Rotation", cinema::Vector(0.0, 0.0, angle));
			_graph.SetInput(turn, "Location", cinema::Vector(0.5 - (0.5 * c - 0.5 * s) + offset.x, 0.5 - (0.5 * s + 0.5 * c) + offset.y, 0.0));
			Set(turn, "Vector", coordinates, false) iferr_return;
			coordinates = Term::Of(NodeOutput { turn, "Vector"_s });
		}
		if (scale != maxon::Vector2d(1.0))
		{
			coordinates = VMath("MULTIPLY", coordinates, Term::Of(cinema::Vector(scale.x, scale.y, 1.0))) iferr_return;
		}
	}
	if (!vector && !projected.box && !mapping.IsPlain())
	{
		const NodeOutput at = placed ? coordinates.out : NodeOutput();
		const NodeOutput tiled = TileTextureTag(_graph, at, mapping) iferr_return;
		if (tiled.IsValid())
		{
			coordinates = Term::Of(tiled);
			placed = true;
		}
	}
	if (placed)
	{
		Set(texture, "Vector", coordinates, false) iferr_return;
	}
	if (projected.box)
		_graph.SetProperty(texture, "projection", "BOX"_s);
	if (projected.clip)
		_graph.SetProperty(texture, "extension", "CLIP"_s);
	const maxon::Float64 wrapU = Value(node, "wrapu", 1.0) iferr_return;
	const maxon::Float64 wrapV = Value(node, "wrapv", 1.0) iferr_return;
	const maxon::Float64 mirrorU = Value(node, "mirroru", 0.0) iferr_return;
	const maxon::Float64 mirrorV = Value(node, "mirrorv", 0.0) iferr_return;
	if (wrapU == 0.0 || wrapV == 0.0 || mirrorU != 0.0 || mirrorV != 0.0)
		Warn(FormatString("the wrap and mirror settings of a Texture node plugged into @ are not translated; the texture repeats.", maxon::String(what)));

	// Its colour adjustments (measured, F13 W18): colour × multiplier + offset; the custom gamma as a power of the colour
	// (Redshift raises the file's decoded value before turning it into the render space: exact in a Basic document and for
	// greys, approximate for colours in an OCIO document).
	Term color = Term::Of(NodeOutput { texture, "Color"_s });
	const maxon::Float64 gamma = Setting(node, "tex0_gamma", 1.0, what, " Texture") iferr_return;
	if (gamma != 1.0 && gamma > 0.0)
	{
		const maxon::Int power = _graph.Add("ShaderNodeGamma") iferr_return;
		_graph.SetInput(power, "Gamma", gamma);
		Set(power, "Color", color, false) iferr_return;
		color = Term::Of(NodeOutput { power, "Color"_s });
	}
	const cinema::Vector multiplier = VectorValue(node, "color_multiplier", cinema::Vector(1.0)) iferr_return;
	const cinema::Vector added = VectorValue(node, "color_offset", cinema::Vector(0.0)) iferr_return;
	const maxon::GraphNode multiplierPort = Port(node, "color_multiplier") iferr_return;
	const maxon::GraphNode addedPort = Port(node, "color_offset") iferr_return;
	const maxon::Bool multiplierWired = IsWired(multiplierPort) iferr_return;
	const maxon::Bool addedWired = IsWired(addedPort) iferr_return;
	if (multiplierWired || addedWired)
		Warn(FormatString("the colour multiplier and offset of a Texture node plugged into @ are wired; only their values are translated.", maxon::String(what)));
	if (multiplier != cinema::Vector(1.0) || added != cinema::Vector(0.0))
	{
		color = VMath("MULTIPLY_ADD", color, Term::Of(multiplier), Term::Of(added)) iferr_return;
	}
	if (color.IsConstant())
		return Output(color, false);
	return color.out;
}

//----------------------------------------------------------------------------------------
// Terms: expressions of Blender nodes. A term is a constant or a node's output; operations on
// constants are folded where the arithmetic is plain (+, -, *, /, min, max), others always make
// a node (Blender folds them itself).
//----------------------------------------------------------------------------------------
maxon::Result<RedshiftNodes::Term> RedshiftNodes::Input(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Bool colorData,
	const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode port = InputPort(rsNode, asset.ToString(), name) iferr_return;
	Term term;
	const PortValue value = ReadValue(port);
	if (value.found)
	{
		term.value = value.isVector ? value.vector : cinema::Vector(value.number);
		term.alpha = value.alpha;
	}
	const Source source = Find(port) iferr_return;
	if (!source.node.IsValid())
		return term;
	const NodeOutput wired = Translate(source, colorData, what, depth + 1) iferr_return;
	if (wired.IsValid())
	{
		term.out = wired;
		term.alpha = 1.0;
	}
	return term;
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::PortTerm(const maxon::GraphNode& port, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	Term term;
	const PortValue value = ReadValue(port);
	if (value.found)
	{
		term.value = value.isVector ? value.vector : cinema::Vector(value.number);
		term.alpha = value.alpha;
	}
	const Source source = Find(port) iferr_return;
	if (!source.node.IsValid())
		return term;
	const NodeOutput wired = Translate(source, colorData, what, depth + 1) iferr_return;
	if (wired.IsValid())
	{
		term.out = wired;
		term.alpha = 1.0;
	}
	return term;
}

maxon::Float64 RedshiftNodes::PortNumber(const maxon::GraphNode& port, maxon::Float64 fallback)
{
	const PortValue value = ReadValue(port);
	if (!value.found)
		return fallback;
	return value.isVector ? value.vector.x : value.number;
}

maxon::Result<maxon::GraphNode> RedshiftNodes::Port(const maxon::GraphNode& rsNode, const maxon::Char* name)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	return InputPort(rsNode, asset.ToString(), name);
}

maxon::Result<maxon::Bool> RedshiftNodes::IsWired(const maxon::GraphNode& port)
{
	iferr_scope;
	const Source source = Find(port) iferr_return;
	return source.node.IsValid();
}

maxon::Result<maxon::Float64> RedshiftNodes::Value(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Float64 fallback)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode port = InputPort(rsNode, asset.ToString(), name) iferr_return;
	const PortValue value = ReadValue(port);
	if (!value.found)
		return fallback;
	return value.isVector ? value.vector.x : value.number;
}

maxon::Result<cinema::Vector> RedshiftNodes::VectorValue(const maxon::GraphNode& rsNode, const maxon::Char* name, const cinema::Vector& fallback)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode port = InputPort(rsNode, asset.ToString(), name) iferr_return;
	const PortValue value = ReadValue(port);
	if (!value.found)
		return fallback;
	return value.isVector ? value.vector : cinema::Vector(value.number);
}

maxon::Result<maxon::Float64> RedshiftNodes::Setting(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Float64 fallback,
	const maxon::Char* what, const maxon::Char* label)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode port = InputPort(rsNode, asset.ToString(), name) iferr_return;
	const Source wired = Find(port) iferr_return;
	if (wired.node.IsValid())
		Warn(FormatString("the \"@\" of a@ node plugged into @ is wired; only its value is translated.", maxon::String(name), maxon::String(label), maxon::String(what)));
	const PortValue value = ReadValue(port);
	if (!value.found)
		return fallback;
	return value.isVector ? value.vector.x : value.number;
}

maxon::Result<void> RedshiftNodes::Set(maxon::Int node, const maxon::Char* socket, const Term& term, maxon::Bool scalar)
{
	iferr_scope;
	if (term.IsConstant())
	{
		if (scalar)
			_graph.SetInput(node, socket, term.value.x);
		else
			_graph.SetInput(node, socket, term.value);
		return maxon::OK;
	}
	_graph.Link(term.out, node, socket) iferr_return;
	return maxon::OK;
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Math(const maxon::Char* operation, const Term& a, const Term& b, const Term& c)
{
	iferr_scope;
	const maxon::String op(operation);
	if (a.IsConstant() && b.IsConstant())
	{
		const maxon::Float64 x = a.value.x, y = b.value.x;
		if (op == "ADD"_s)
			return Term::Number(x + y);
		if (op == "SUBTRACT"_s)
			return Term::Number(x - y);
		if (op == "MULTIPLY"_s)
			return Term::Number(x * y);
		if (op == "DIVIDE"_s)
			return Term::Number(y != 0.0 ? x / y : 0.0);
		if (op == "MINIMUM"_s)
			return Term::Number(maxon::Min(x, y));
		if (op == "MAXIMUM"_s)
			return Term::Number(maxon::Max(x, y));
	}
	// x * 1, x + 0, x - 0 change nothing.
	if (!a.IsConstant() && b.IsConstant() && ((op == "MULTIPLY"_s && b.value.x == 1.0) || ((op == "ADD"_s || op == "SUBTRACT"_s) && b.value.x == 0.0)))
		return a;
	const maxon::Int node = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(node, "operation", op);
	Set(node, "Value", a, true) iferr_return;
	Set(node, "Value_001", b, true) iferr_return;
	Set(node, "Value_002", c, true) iferr_return;
	return Term::Of(NodeOutput { node, "Value"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::VMath(const maxon::Char* operation, const Term& a, const Term& b, const Term& c)
{
	iferr_scope;
	const maxon::String op(operation);
	if (a.IsConstant() && b.IsConstant() && c.IsConstant())
	{
		const cinema::Vector& x = a.value;
		const cinema::Vector& y = b.value;
		if (op == "ADD"_s)
			return Term::Of(x + y);
		if (op == "SUBTRACT"_s)
			return Term::Of(x - y);
		if (op == "MULTIPLY"_s)
			return Term::Of(cinema::Vector(x.x * y.x, x.y * y.y, x.z * y.z));
		if (op == "SCALE"_s)
			return Term::Of(x * y.x);
		if (op == "MULTIPLY_ADD"_s)
			return Term::Of(cinema::Vector(x.x * y.x + c.value.x, x.y * y.y + c.value.y, x.z * y.z + c.value.z));
	}
	if (op == "SCALE"_s && b.IsConstant())
	{
		if (b.value.x == 1.0)
			return a;
		if (b.value.x == 0.0)
			return Term::Of(cinema::Vector(0.0));
	}
	if (op == "ADD"_s && b.IsConstant() && b.value == cinema::Vector(0.0))
		return a;
	if (op == "ADD"_s && a.IsConstant() && a.value == cinema::Vector(0.0))
		return b;
	const maxon::Int node = _graph.Add("ShaderNodeVectorMath") iferr_return;
	_graph.SetProperty(node, "operation", op);
	Set(node, "Vector", a, false) iferr_return;
	if (op == "SCALE"_s)
	{
		Set(node, "Scale", b, true) iferr_return;
	}
	else
	{
		Set(node, "Vector_001", b, false) iferr_return;
		Set(node, "Vector_002", c, false) iferr_return;
	}
	const maxon::Bool number = op == "DOT_PRODUCT"_s || op == "LENGTH"_s || op == "DISTANCE"_s;
	return Term::Of(NodeOutput { node, number ? "Value"_s : "Vector"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::MixColor(const maxon::Char* blend, const Term& factor, const Term& a, const Term& b)
{
	iferr_scope;
	const maxon::Int node = _graph.Add("ShaderNodeMix") iferr_return;
	_graph.SetProperty(node, "data_type", "RGBA"_s);
	_graph.SetProperty(node, "blend_type", maxon::String(blend));
	_graph.SetProperty(node, "clamp_factor", false);
	Set(node, "Factor_Float", factor, true) iferr_return;
	Set(node, "A_Color", a, false) iferr_return;
	Set(node, "B_Color", b, false) iferr_return;
	return Term::Of(NodeOutput { node, "Result_Color"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Component(const Term& vector, maxon::Int index)
{
	iferr_scope;
	if (vector.IsConstant())
		return Term::Number(index == 0 ? vector.value.x : index == 1 ? vector.value.y : vector.value.z);
	const maxon::Int node = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
	_graph.Link(vector.out, node, "Vector") iferr_return;
	return Term::Of(NodeOutput { node, index == 0 ? "X"_s : index == 1 ? "Y"_s : "Z"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Combine(const Term& x, const Term& y, const Term& z)
{
	iferr_scope;
	if (x.IsConstant() && y.IsConstant() && z.IsConstant())
		return Term::Of(cinema::Vector(x.value.x, y.value.x, z.value.x));
	const maxon::Int node = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	Set(node, "X", x, true) iferr_return;
	Set(node, "Y", y, true) iferr_return;
	Set(node, "Z", z, true) iferr_return;
	return Term::Of(NodeOutput { node, "Vector"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::PerComponent(const Term& a, const Term& b, maxon::Result<Term> (RedshiftNodes::*function)(const Term&, const Term&))
{
	iferr_scope;
	// One Separate XYZ per wired vector, shared by the three components.
	Term splitA = a, splitB = b;
	maxon::Int nodeA = NOTOK, nodeB = NOTOK;
	if (!a.IsConstant())
	{
		nodeA = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
		_graph.Link(a.out, nodeA, "Vector") iferr_return;
	}
	if (!b.IsConstant())
	{
		nodeB = _graph.Add("ShaderNodeSeparateXYZ") iferr_return;
		_graph.Link(b.out, nodeB, "Vector") iferr_return;
	}
	Term parts[3];
	static const maxon::Char* const AXES[3] = { "X", "Y", "Z" };
	for (maxon::Int i = 0; i < 3; ++i)
	{
		const Term x = nodeA != NOTOK ? Term::Of(NodeOutput { nodeA, maxon::String(AXES[i]) }) : Term::Number(i == 0 ? a.value.x : i == 1 ? a.value.y : a.value.z);
		const Term y = nodeB != NOTOK ? Term::Of(NodeOutput { nodeB, maxon::String(AXES[i]) }) : Term::Number(i == 0 ? b.value.x : i == 1 ? b.value.y : b.value.z);
		parts[i] = (this->*function)(x, y) iferr_return;
	}
	return Combine(parts[0], parts[1], parts[2]);
}

// Perlin's bias: x^(log b / log 0.5). Redshift's Bias, measured: 0.3 at 0.7 gives 0.538, 0.8 at 0.2 gives 0.596.
maxon::Result<RedshiftNodes::Term> RedshiftNodes::Bias(const Term& x, const Term& b)
{
	iferr_scope;
	const Term exponent = Math("LOGARITHM", b, Term::Number(0.5)) iferr_return;
	return Math("POWER", x, exponent);
}

// Perlin's gain: bias(2x, 1 - g) / 2 below 0.5, 1 - bias(2 - 2x, 1 - g) / 2 from 0.5 on (measured:
// 0.3 at 0.7 gives 0.206, 0.8 at 0.7 gives 0.899, 0.2 at 0.2 gives 0.372).
maxon::Result<RedshiftNodes::Term> RedshiftNodes::Gain(const Term& x, const Term& g)
{
	iferr_scope;
	const Term flipped = Math("SUBTRACT", Term::Number(1.0), g) iferr_return;
	const Term exponent = Math("LOGARITHM", flipped, Term::Number(0.5)) iferr_return;
	const Term twice = Math("MULTIPLY", x, Term::Number(2.0)) iferr_return;
	const Term lowPower = Math("POWER", twice, exponent) iferr_return;
	const Term low = Math("MULTIPLY", lowPower, Term::Number(0.5)) iferr_return;
	const Term rest = Math("MULTIPLY_ADD", x, Term::Number(-2.0), Term::Number(2.0)) iferr_return;
	const Term highPower = Math("POWER", rest, exponent) iferr_return;
	const Term high = Math("MULTIPLY_ADD", highPower, Term::Number(-0.5), Term::Number(1.0)) iferr_return;
	const Term below = Math("LESS_THAN", x, Term::Number(0.5)) iferr_return;
	const Term step = Math("SUBTRACT", low, high) iferr_return;
	return Math("MULTIPLY_ADD", below, step, high);
}

maxon::Result<NodeOutput> RedshiftNodes::Output(const Term& term, maxon::Bool scalar)
{
	iferr_scope;
	if (!term.IsConstant())
		return term.out;
	if (scalar)
	{
		const maxon::Int node = _graph.Add("ShaderNodeValue") iferr_return;
		_graph.SetOutputJson(node, "Value", FormatString("@", term.value.x));
		return NodeOutput { node, "Value"_s };
	}
	const maxon::Int node = _graph.Add("ShaderNodeCombineXYZ") iferr_return;
	_graph.SetInput(node, "X", term.value.x);
	_graph.SetInput(node, "Y", term.value.y);
	_graph.SetInput(node, "Z", term.value.z);
	return NodeOutput { node, "Vector"_s };
}

//----------------------------------------------------------------------------------------
// Redshift's blend modes on two colours, numbered as the Color Composite's "comp_mode" (the Color
// Layer's modes are the same functions, its "Normal" being 1 here). Measured on constant colours
// rendered by Redshift (docs/reference/c4d-sdk.md, "Redshift nodes"): all but Soft Light are
// Blender's Mix blend types; Hard Light is Overlay with the inputs swapped, Divide is blend / base,
// Soft Light is Photoshop's (base - (1 - 2s) base (1 - base) below 0.5, base + (2s - 1)(sqrt(base) -
// base) above), Burn and Dodge clamp at 0 and 1 as Blender's do.
//----------------------------------------------------------------------------------------
maxon::Result<RedshiftNodes::Term> RedshiftNodes::Blend(maxon::Int mode, const Term& base, const Term& blend)
{
	iferr_scope;
	const Term one = Term::Number(1.0);
	switch (mode)
	{
		case 0: return base;
		case 1: return blend;
		case 2: return MixColor("MIX", Term::Number(0.5), base, blend);
		case 3: return MixColor("ADD", one, base, blend);
		case 4: return MixColor("SUBTRACT", one, base, blend);
		case 5: return MixColor("MULTIPLY", one, base, blend);
		case 6: return MixColor("DIFFERENCE", one, base, blend);
		case 7: return MixColor("LIGHTEN", one, base, blend);
		case 8: return MixColor("DARKEN", one, base, blend);
		case 9: return MixColor("SCREEN", one, base, blend);
		case 10: return MixColor("OVERLAY", one, blend, base);
		case 11:
		{
			// d = b (1 - b) below 0.5, sqrt(b) - b above; result b + (2s - 1) d.
			const Term rootOfBase = VMath("POWER", base, Term::Of(cinema::Vector(0.5))) iferr_return;
			const Term high = VMath("SUBTRACT", rootOfBase, base) iferr_return;
			const Term inverse = VMath("SUBTRACT", Term::Of(cinema::Vector(1.0)), base) iferr_return;
			const Term low = VMath("MULTIPLY", base, inverse) iferr_return;
			const Term centred = VMath("SUBTRACT", blend, Term::Of(cinema::Vector(0.5))) iferr_return;
			const Term sign = VMath("SIGN", centred) iferr_return;
			const Term above = VMath("MAXIMUM", sign, Term::Of(cinema::Vector(0.0))) iferr_return;
			const Term difference = VMath("SUBTRACT", high, low) iferr_return;
			const Term d = VMath("MULTIPLY_ADD", above, difference, low) iferr_return;
			const Term t = VMath("MULTIPLY_ADD", blend, Term::Of(cinema::Vector(2.0)), Term::Of(cinema::Vector(-1.0))) iferr_return;
			return VMath("MULTIPLY_ADD", t, d, base);
		}
		case 12: return MixColor("BURN", one, base, blend);
		case 13: return MixColor("DODGE", one, base, blend);
		case 14: return MixColor("OVERLAY", one, base, blend);
		case 15: return MixColor("EXCLUSION", one, base, blend);
		case 16: return MixColor("DIVIDE", one, blend, base);
		default: return base;
	}
}

//----------------------------------------------------------------------------------------
// More math: inverse trigonometry, logarithm, bias and gain, and the colour and vector
// versions of the scalar operations. Measured against Redshift's render of constants
// (docs/reference/c4d-sdk.md, "Redshift nodes").
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::MoreMath(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;
	auto in = [&](const maxon::Char* port) -> maxon::Result<Term> { return Input(rs, port, false, what, depth); };
	auto colorIn = [&](const maxon::Char* port) -> maxon::Result<Term> { return Input(rs, port, colorData, what, depth); };
	const Term e = Term::Of(cinema::Vector(maxon::Exp(1.0)));

	// Arc functions answer in radians; with "is radians" off Redshift multiplies the answer by
	// pi / 180 (measured: arccos 0.5 gives 1.047, and 0.0183 with the switch off).
	if (name == "rsmathacos"_s || name == "rsmathasin"_s || name == "rsmathatan"_s)
	{
		const Term x = in("input") iferr_return;
		Term y = Math(name == "rsmathacos"_s ? "ARCCOSINE" : name == "rsmathasin"_s ? "ARCSINE" : "ARCTANGENT", x) iferr_return;
		const maxon::Float64 radians = Value(rs, "isradians", 1.0) iferr_return;
		if (radians == 0.0)
		{
			y = Math("MULTIPLY", y, Term::Number(maxon::PI / 180.0)) iferr_return;
		}
		return Output(y, true);
	}
	if (name == "rsmathatan2"_s)
	{
		// atan2 of x over y (measured: x 0.5, y 0.3 gives 1.030).
		const Term x = in("x") iferr_return;
		const Term y = in("y") iferr_return;
		const Term angle = Math("ARCTAN2", x, y) iferr_return;
		return Output(angle, true);
	}
	if (name == "rsmathlog"_s)
	{
		const Term x = in("input") iferr_return;
		const Term base = in("base") iferr_return;
		const Term y = Math("LOGARITHM", x, base) iferr_return;
		return Output(y, true);
	}
	if (name == "rsmathbias"_s || name == "rsmathgain"_s)
	{
		const maxon::Bool bias = name == "rsmathbias"_s;
		const Term x = in("input") iferr_return;
		const Term k = in(bias ? "bias" : "gain") iferr_return;
		const Term y = (bias ? Bias(x, k) : Gain(x, k)) iferr_return;
		return Output(y, true);
	}

	// Colours: Abs, Exp, Bias, Gain ("Apply To Alpha" concerns the alpha, which Blender's colours lack).
	if (name == "rsmathabscolor"_s || name == "rsmathexpcolor"_s)
	{
		const Term x = colorIn("input") iferr_return;
		const Term y = (name == "rsmathabscolor"_s ? VMath("ABSOLUTE", x) : VMath("POWER", e, x)) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathbiascolor"_s || name == "rsmathgaincolor"_s || name == "rsmathbiasvector"_s || name == "rsmathgainvector"_s)
	{
		const maxon::Bool bias = name == "rsmathbiascolor"_s || name == "rsmathbiasvector"_s;
		const Term x = colorIn("input") iferr_return;
		const Term k = colorIn(bias ? "bias" : "gain") iferr_return;
		const Term y = PerComponent(x, k, bias ? &RedshiftNodes::Bias : &RedshiftNodes::Gain) iferr_return;
		return Output(y, false);
	}

	// Vectors.
	if (name == "rsmathfloorvector"_s || name == "rsmathfracvector"_s || name == "rsmathsignvector"_s)
	{
		const Term x = in("input") iferr_return;
		const Term y = VMath(name == "rsmathfloorvector"_s ? "FLOOR" : name == "rsmathfracvector"_s ? "FRACTION" : "SIGN", x) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathmodvector"_s)
	{
		// Truncated, as fmod (measured: -1.3 mod 0.5 is not 0.2, 1.3 mod -0.5 is 0.3).
		const Term x = in("input") iferr_return;
		const Term d = in("divisor") iferr_return;
		const Term y = VMath("MODULO", x, d) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathpowvector"_s)
	{
		const Term x = in("base") iferr_return;
		const Term p = in("exponent") iferr_return;
		const Term y = VMath("POWER", x, p) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathexpvector"_s)
	{
		const Term x = in("input") iferr_return;
		const Term y = VMath("POWER", e, x) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathlnvector"_s || name == "rsmathlogvector"_s)
	{
		const Term x = in("input") iferr_return;
		Term base = e;
		if (name == "rsmathlogvector"_s)
		{
			base = in("base") iferr_return;
		}
		const Term y = PerComponent(x, base, &RedshiftNodes::Logarithm) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathsqrtvector"_s)
	{
		// Redshift's Vector Sqrt gives 0 in z whatever the input (measured: (0.5, 0.25, 4) gives
		// (0.707, 0.5, 0)); kept, so that the picture is Redshift's.
		const Term x = in("input") iferr_return;
		const Term root = VMath("POWER", x, Term::Of(cinema::Vector(0.5))) iferr_return;
		const Term y = VMath("MULTIPLY", root, Term::Of(cinema::Vector(1.0, 1.0, 0.0))) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathrcpvector"_s || name == "rsmathinvvector"_s)
	{
		const Term x = in("input") iferr_return;
		const Term y = VMath(name == "rsmathrcpvector"_s ? "DIVIDE" : "SUBTRACT", Term::Of(cinema::Vector(1.0)), x) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathnegvector"_s)
	{
		const Term x = in("input") iferr_return;
		const Term y = VMath("SCALE", x, Term::Number(-1.0)) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathsaturatevector"_s)
	{
		const Term x = in("input") iferr_return;
		const Term low = VMath("MAXIMUM", x, Term::Of(cinema::Vector(0.0))) iferr_return;
		const Term y = VMath("MINIMUM", low, Term::Of(cinema::Vector(1.0))) iferr_return;
		return Output(y, false);
	}
	if (name == "rsmathrangevector"_s || name == "rscolorrange"_s)
	{
		// Map Range per component; Clamp holds the result between the new minimum and maximum.
		const maxon::Bool color = name == "rscolorrange"_s;
		const maxon::Int node = _graph.Add("ShaderNodeMapRange") iferr_return;
		_graph.SetProperty(node, "data_type", "FLOAT_VECTOR"_s);
		const Term x = Input(rs, "input", color && colorData, what, depth) iferr_return;
		Set(node, "Vector", x, false) iferr_return;
		const Term oldMin = in("old_min") iferr_return;
		Set(node, "From_Min_FLOAT3", oldMin, false) iferr_return;
		const Term oldMax = in("old_max") iferr_return;
		Set(node, "From_Max_FLOAT3", oldMax, false) iferr_return;
		const Term newMin = in("new_min") iferr_return;
		Set(node, "To_Min_FLOAT3", newMin, false) iferr_return;
		const Term newMax = in("new_max") iferr_return;
		Set(node, "To_Max_FLOAT3", newMax, false) iferr_return;
		const maxon::Float64 clamp = Value(rs, "clamp", 1.0) iferr_return;
		_graph.SetProperty(node, "clamp", clamp != 0.0);
		return NodeOutput { node, "Vector"_s };
	}

	// Unit Conversion: the value times a length (measured: "Scene Units" multiplies by the
	// dynamic unit length; "Fixed" by the fixed length times the unit in centimetres: millimetres
	// 0.1, inches 2.54). Redshift's numbers are in Cinema 4D's units, so are these.
	if (name == "unitconversion"_s)
	{
		static const maxon::Float64 CENTIMETRES[] = { 100000.0, 100.0, 1.0, 0.1, 1e-4, 1e-7, 160934.4, 91.44, 30.48, 2.54 };
		const maxon::Float64 mode = Value(rs, "mode", 0.0) iferr_return;
		maxon::Float64 factor = 1.0;
		if (mode == 0.0)
		{
			factor = Setting(rs, "dynamicunitlength", 1.0, what, " Unit Conversion") iferr_return;
		}
		else
		{
			const maxon::Float64 unit = Value(rs, "unit", 2.0) iferr_return;
			const maxon::Int index = maxon::Int(unit);
			const maxon::Float64 length = Setting(rs, "fixedunitlength", 1.0, what, " Unit Conversion") iferr_return;
			factor = length * (index >= 0 && index < 10 ? CENTIMETRES[index] : 1.0);
		}
		if (source.output == "outvector"_s)
		{
			const Term x = in("vectorvalue") iferr_return;
			const Term y = VMath("SCALE", x, Term::Number(factor)) iferr_return;
			return Output(y, false);
		}
		const Term x = in("scalarvalue") iferr_return;
		const Term y = Math("MULTIPLY", x, Term::Number(factor)) iferr_return;
		return Output(y, true);
	}

	handled = false;
	return NodeOutput();
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Logarithm(const Term& x, const Term& base)
{
	return Math("LOGARITHM", x, base);
}

#define RS_LAYER(n) { "layer" #n "_enable", "layer" #n "_color", "layer" #n "_mask", "layer" #n "_blend_mode", "layer" #n "_overlay_mode", "layer" #n "_color_premult" }
static const maxon::Char* const LAYER_PORTS[7][6] = { RS_LAYER(1), RS_LAYER(2), RS_LAYER(3), RS_LAYER(4), RS_LAYER(5), RS_LAYER(6), RS_LAYER(7) };
#undef RS_LAYER

//----------------------------------------------------------------------------------------
// Colour nodes: Color Composite, Color Layer, Golaem HSL, Shader Switch. The rules were
// measured on constant colours rendered by Redshift (docs/reference/c4d-sdk.md, "Redshift nodes").
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::ColorNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;

	// Color Composite: the colours premultiplied by their alpha, then the mode on those; Mask,
	// Stencil and Matte use the blend colour's alpha. A wired colour counts as opaque. Before 2027
	// Redshift takes a colour port's value as premultiplied already (sdk_compat.h).
	if (name == "rscolorcomposite"_s)
	{
		Term base = Input(rs, "base_color", colorData, what, depth) iferr_return;
		Term blend = Input(rs, "blend_color", colorData, what, depth) iferr_return;
		if (REDSHIFT_STRAIGHT_COLORS && base.IsConstant())
			base.value *= base.alpha;
		if (REDSHIFT_STRAIGHT_COLORS && blend.IsConstant())
			blend.value *= blend.alpha;
		const maxon::Float64 modeValue = Value(rs, "comp_mode", 0.0) iferr_return;
		const maxon::Int mode = maxon::Int(modeValue);
		Term result;
		if (mode == 17 || mode == 18)
		{
			result = VMath("SCALE", base, Term::Number(mode == 17 ? blend.alpha : 1.0 - blend.alpha)) iferr_return;
		}
		else if (mode == 19)
		{
			result = MixColor("MIX", Term::Number(blend.alpha), base, blend) iferr_return;
		}
		else
		{
			result = Blend(mode, base, blend) iferr_return;
		}
		return Output(result, false);
	}

	// Color Layer: a base and up to seven layers, each composited onto what is below by the
	// separable blend modes of the W3C's compositing rules (measured): with Ub, ab the colour
	// (not premultiplied) and alpha below, Us, as the layer's (as times its mask), the result is
	// as (1 - ab) Us + ab (1 - as) Ub + as ab B(Ub, Us), premultiplied, alpha as + ab (1 - as).
	// In advanced mode the overlay mode (Porter and Duff's operators) says which of the three
	// regions shows, and "premultiplied" off multiplies a colour by its alpha once more. Before 2027
	// Redshift takes a colour port's value as premultiplied already (sdk_compat.h): U = c / alpha.
	if (name == "rscolorlayer"_s)
	{
		auto straight = [](Term& color)
		{
			if (!REDSHIFT_STRAIGHT_COLORS && color.IsConstant() && color.alpha > 0.0)
				color.value /= color.alpha;
		};
		const maxon::Float64 advancedValue = Value(rs, "advanced_mode", 0.0) iferr_return;
		const maxon::Bool advanced = advancedValue != 0.0;
		Term colour = Input(rs, "base_color", colorData, what, depth) iferr_return;
		straight(colour);
		Term alpha = Term::Number(colour.alpha);
		const maxon::Float64 basePremultiplied = Value(rs, "base_color_premult", 1.0) iferr_return;
		if (advanced && basePremultiplied == 0.0)
		{
			colour = VMath("SCALE", colour, alpha) iferr_return;
		}
		// The colour below, premultiplied: ab Ub.
		Term premultiplied = VMath("SCALE", colour, alpha) iferr_return;
		for (maxon::Int layer = 1; layer <= 7; ++layer)
		{
			const maxon::Char* const* const ports = LAYER_PORTS[layer - 1];
			const maxon::Char* const colorName = ports[1];
			const maxon::Char* const maskName = ports[2];
			const maxon::Float64 enabled = Setting(rs, ports[0], layer == 1 ? 1.0 : 0.0, what, " Color Layer") iferr_return;
			if (enabled == 0.0)
				continue;
			Term us = Input(rs, colorName, colorData, what, depth) iferr_return;
			straight(us);
			const Term mask = Input(rs, maskName, false, what, depth) iferr_return;
			const Term as = Math("MULTIPLY", Term::Number(us.alpha), mask) iferr_return;
			const maxon::Float64 layerPremultiplied = Value(rs, ports[5], 1.0) iferr_return;
			if (advanced && layerPremultiplied == 0.0)
			{
				us = VMath("SCALE", us, Term::Number(us.alpha)) iferr_return;
			}
			// Which regions show: the layer alone (s), what is below alone (d), and where both are.
			maxon::Bool s = true, d = true;
			maxon::Int both = 1;        // 1: the layer's colour, 2: the colour below, 0: nothing
			if (advanced)
			{
				static const maxon::Int OPERATORS[12][3] =
				{
					{ 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 2 },   // Src, Atop, Over, In, Out, Dest
					{ 1, 0, 2 }, { 1, 1, 2 }, { 0, 0, 2 }, { 0, 1, 0 }, { 0, 0, 0 }, { 1, 1, 0 }    // DestAtop, DestOver, DestIn, DestOut, Clear, Xor
				};
				const maxon::Float64 overlay = Value(rs, ports[4], 2.0) iferr_return;
				const maxon::Int op = maxon::ClampValue(maxon::Int(overlay), maxon::Int(0), maxon::Int(11));
				s = OPERATORS[op][0] != 0;
				d = OPERATORS[op][1] != 0;
				both = OPERATORS[op][2];
			}
			const maxon::Float64 blendValue = Value(rs, ports[3], 0.0) iferr_return;
			const maxon::Int blend = maxon::Int(blendValue) + 1;
			// The colour below, not premultiplied (0 where nothing is).
			Term ub = premultiplied;
			if (!(alpha.IsConstant() && alpha.value.x == 1.0))
			{
				const Term alphas = Combine(alpha, alpha, alpha) iferr_return;
				ub = VMath("DIVIDE", premultiplied, alphas) iferr_return;
			}
			Term mixed = Term::Of(cinema::Vector(0.0));
			if (both != 0 && blend == 1)
				mixed = both == 1 ? us : ub;
			if (both != 0 && blend != 1)
			{
				mixed = Blend(blend, ub, us) iferr_return;
			}
			const Term notBelow = Math("SUBTRACT", Term::Number(1.0), alpha) iferr_return;
			const Term notLayer = Math("SUBTRACT", Term::Number(1.0), as) iferr_return;
			Term layerOnly = Term::Number(0.0), belowOnly = Term::Number(0.0), overlap = Term::Number(0.0);
			if (s)
			{
				layerOnly = Math("MULTIPLY", as, notBelow) iferr_return;
			}
			if (d)
			{
				belowOnly = Math("MULTIPLY", alpha, notLayer) iferr_return;
			}
			if (both != 0)
			{
				overlap = Math("MULTIPLY", as, alpha) iferr_return;
			}
			const Term a1 = VMath("SCALE", us, layerOnly) iferr_return;
			const Term a2 = VMath("SCALE", ub, belowOnly) iferr_return;
			const Term a3 = VMath("SCALE", mixed, overlap) iferr_return;
			const Term sum = VMath("ADD", a1, a2) iferr_return;
			premultiplied = VMath("ADD", sum, a3) iferr_return;
			const Term alphaSum = Math("ADD", layerOnly, belowOnly) iferr_return;
			alpha = Math("ADD", alphaSum, overlap) iferr_return;
		}
		return Output(premultiplied, false);
	}

	// Golaem HSL: in HSL, the hue turned by h half turns (h held to -1..1), saturation and
	// lightness moved by s and l towards 1 when positive and towards 0 when negative (measured).
	if (name == "golaemhsl"_s)
	{
		const Term color = Input(rs, "incolor", colorData, what, depth) iferr_return;
		const maxon::Int split = _graph.Add("ShaderNodeSeparateColor") iferr_return;
		_graph.SetProperty(split, "mode", "HSL"_s);
		Set(split, "Color", color, false) iferr_return;
		const Term h = Input(rs, "h", false, what, depth) iferr_return;
		const Term s = Input(rs, "s", false, what, depth) iferr_return;
		const Term l = Input(rs, "l", false, what, depth) iferr_return;
		const Term low = Math("MAXIMUM", h, Term::Number(-1.0)) iferr_return;
		const Term turn = Math("MINIMUM", low, Term::Number(1.0)) iferr_return;
		const Term shifted = Math("MULTIPLY_ADD", turn, Term::Number(0.5), Term::Of(NodeOutput { split, "Red"_s })) iferr_return;
		const Term hue = Math("FRACT", shifted) iferr_return;
		auto towards = [&](const Term& x, const Term& a) -> maxon::Result<Term>
		{
			iferr_scope;
			if (a.IsConstant())
			{
				const maxon::Float64 k = maxon::ClampValue(a.value.x, -1.0, 1.0);
				if (k >= 0.0)
					return Math("MULTIPLY_ADD", x, Term::Number(1.0 - k), Term::Number(k));
				return Math("MULTIPLY", x, Term::Number(1.0 + k));
			}
			// x + a (x + [a > 0] (1 - 2x))
			const Term up = Math("GREATER_THAN", a, Term::Number(0.0)) iferr_return;
			const Term away = Math("MULTIPLY_ADD", x, Term::Number(-2.0), Term::Number(1.0)) iferr_return;
			const Term room = Math("MULTIPLY_ADD", up, away, x) iferr_return;
			return Math("MULTIPLY_ADD", a, room, x);
		};
		const Term saturation = towards(Term::Of(NodeOutput { split, "Green"_s }), s) iferr_return;
		const Term lightness = towards(Term::Of(NodeOutput { split, "Blue"_s }), l) iferr_return;
		const maxon::Int join = _graph.Add("ShaderNodeCombineColor") iferr_return;
		_graph.SetProperty(join, "mode", "HSL"_s);
		Set(join, "Red", hue, true) iferr_return;
		Set(join, "Green", saturation, true) iferr_return;
		Set(join, "Blue", lightness, true) iferr_return;
		return NodeOutput { join, "Color"_s };
	}

	// Shader Switch: input number selector - offset (0 to 9), else the default (measured).
	if (name == "rsshaderswitch"_s)
	{
		if (source.output == "outclosure"_s)
		{
			Warn(FormatString("the closure output of a Shader Switch plugged into @ is not translated; the value set on the input is used.", maxon::String(what)));
			return NodeOutput();
		}
		const maxon::Float64 selector = Setting(rs, "selector", 0.0, what, " Shader Switch") iferr_return;
		const maxon::Float64 offset = Setting(rs, "selector_offset", 0.0, what, " Shader Switch") iferr_return;
		const maxon::Int index = maxon::Int(selector) - maxon::Int(offset);
		static const maxon::Char* const SHADERS[] = { "shader0", "shader1", "shader2", "shader3", "shader4", "shader5", "shader6", "shader7", "shader8", "shader9" };
		const maxon::Char* const port = index >= 0 && index <= 9 ? SHADERS[index] : "default_shader";
		const Term chosen = Input(rs, port, colorData, what, depth) iferr_return;
		return Output(chosen, false);
	}

	handled = false;
	return NodeOutput();
}

// A vector of Blender's world space in Redshift's: Cinema 4D's axes and units (positions), in the
// space the State node's "trans_space" names (0 world, 1 object, 2 camera; 3, a reference object,
// is treated as world by the caller).
maxon::Result<RedshiftNodes::Term> RedshiftNodes::ToCinema(const Term& vector, maxon::Bool point, maxon::Int space)
{
	iferr_scope;
	Term value = vector;
	if (space == 1 || space == 2)
	{
		const maxon::Int transform = _graph.Add("ShaderNodeVectorTransform") iferr_return;
		_graph.SetProperty(transform, "vector_type", point ? "POINT"_s : "VECTOR"_s);
		_graph.SetProperty(transform, "convert_from", "WORLD"_s);
		_graph.SetProperty(transform, "convert_to", space == 1 ? "OBJECT"_s : "CAMERA"_s);
		Set(transform, "Vector", value, false) iferr_return;
		value = Term::Of(NodeOutput { transform, "Vector"_s });
	}
	// Blender's (x, y, z) is Cinema 4D's (x, z, y) in metres (coords.h). A Mapping node scales and
	// then turns: (kx, ky, -kz) turned a quarter about x is (kx, kz, ky). Cycles' camera space is
	// Cinema 4D's (x right, y up, z forward, measured), so there only the units change.
	const maxon::Float64 k = point ? 1.0 / Coords::FromDocument(_exporter.GetDocument()).GetScale() : 1.0;
	const maxon::Int mapping = _graph.Add("ShaderNodeMapping") iferr_return;
	_graph.SetProperty(mapping, "vector_type", "POINT"_s);
	Set(mapping, "Vector", value, false) iferr_return;
	_graph.SetInput(mapping, "Scale", cinema::Vector(k, k, space == 2 ? k : -k));
	if (space != 2)
		_graph.SetInput(mapping, "Rotation", cinema::Vector(maxon::PI05, 0.0, 0.0));
	return Term::Of(NodeOutput { mapping, "Vector"_s });
}

// The surface's tangent along the UV map and the bitangent, in Blender's world space.
maxon::Result<RedshiftNodes::Term> RedshiftNodes::UvTangent()
{
	iferr_scope;
	const maxon::Int tangent = _graph.Add("ShaderNodeTangent") iferr_return;
	_graph.SetProperty(tangent, "direction_type", "UV_MAP"_s);
	return Term::Of(NodeOutput { tangent, "Tangent"_s });
}

//----------------------------------------------------------------------------------------
// Utility nodes: Fresnel, IOR To Metal Tints, Surface Tangent, State, Vertex Attribute,
// Ray Switch, WireFrame, AO. Measured on spheres rendered by Redshift (docs/reference/c4d-sdk.md,
// "Redshift nodes").
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::UtilityNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;
	const Term one = Term::Number(1.0);

	// Fresnel: F by the exact dielectric formula (Blender's Fresnel node), by (1 - cos)^curve
	// without IOR, or by the conductor formula with extinction; the result is facing (1 - F) +
	// perpendicular F. "Correct intensity" (dielectric and conductor) replaces the facing colour by
	// its hue at the brightness of F at normal incidence: facing / max(facing) * F0 (measured).
	if (name == "fresnel"_s)
	{
		const Term facing = Input(rs, "facing_color", colorData, what, depth) iferr_return;
		const Term perpendicular = Input(rs, "perp_color", colorData, what, depth) iferr_return;
		const maxon::Float64 type = Value(rs, "fresnel_type", 0.0) iferr_return;
		const maxon::Float64 useIor = Value(rs, "fresnel_useior", 1.0) iferr_return;
		const maxon::Float64 correct = Value(rs, "correct_intensity", 1.0) iferr_return;
		const Term ior = Input(rs, "ior", false, what, depth) iferr_return;
		{
			const maxon::GraphNode bump = InputPort(rs, RS_NODE "fresnel"_s, "bump_input") iferr_return;
			const Source wired = Find(bump) iferr_return;
			if (wired.node.IsValid())
				Warn(FormatString("the bump input of a Fresnel node plugged into @ is not translated; the surface's own normal is used.", maxon::String(what)));
		}
		// cos of the angle to the normal: 1 - Layer Weight's Facing (blend 0.5).
		const maxon::Int layer = _graph.Add("ShaderNodeLayerWeight") iferr_return;
		const Term cosine = Math("SUBTRACT", one, Term::Of(NodeOutput { layer, "Facing"_s })) iferr_return;
		Term f, f0;
		maxon::Bool corrected = correct != 0.0;
		if (type == 1.0)
		{
			// Conductor: (Rs + Rp) / 2 with Rp = (a c^2 - 2nc + 1) / (a c^2 + 2nc + 1), Rs = (a - 2nc + c^2) / (a + 2nc + c^2), a = n^2 + k^2.
			const Term k = Input(rs, "extinction_coeff", false, what, depth) iferr_return;
			const Term n2 = Math("MULTIPLY", ior, ior) iferr_return;
			const Term a = Math("MULTIPLY_ADD", k, k, n2) iferr_return;
			const Term c2 = Math("MULTIPLY", cosine, cosine) iferr_return;
			const Term nc = Math("MULTIPLY", ior, cosine) iferr_return;
			const Term twoNc = Math("MULTIPLY", nc, Term::Number(2.0)) iferr_return;
			const Term ac2 = Math("MULTIPLY", a, c2) iferr_return;
			const Term ac2Plus1 = Math("ADD", ac2, one) iferr_return;
			const Term rpTop = Math("SUBTRACT", ac2Plus1, twoNc) iferr_return;
			const Term rpBottom = Math("ADD", ac2Plus1, twoNc) iferr_return;
			const Term rp = Math("DIVIDE", rpTop, rpBottom) iferr_return;
			const Term aPlusC2 = Math("ADD", a, c2) iferr_return;
			const Term rsTop = Math("SUBTRACT", aPlusC2, twoNc) iferr_return;
			const Term rsBottom = Math("ADD", aPlusC2, twoNc) iferr_return;
			const Term rsValue = Math("DIVIDE", rsTop, rsBottom) iferr_return;
			const Term sum = Math("ADD", rp, rsValue) iferr_return;
			f = Math("MULTIPLY", sum, Term::Number(0.5)) iferr_return;
			// At normal incidence: ((n - 1)^2 + k^2) / ((n + 1)^2 + k^2).
			const Term below = Math("SUBTRACT", ior, one) iferr_return;
			const Term above = Math("ADD", ior, one) iferr_return;
			const Term k2 = Math("MULTIPLY", k, k) iferr_return;
			const Term top = Math("MULTIPLY_ADD", below, below, k2) iferr_return;
			const Term bottom = Math("MULTIPLY_ADD", above, above, k2) iferr_return;
			f0 = Math("DIVIDE", top, bottom) iferr_return;
		}
		else if (useIor != 0.0)
		{
			const maxon::Int fresnel = _graph.Add("ShaderNodeFresnel") iferr_return;
			Set(fresnel, "IOR", ior, true) iferr_return;
			f = Term::Of(NodeOutput { fresnel, "Fac"_s });
			const Term below = Math("SUBTRACT", ior, one) iferr_return;
			const Term above = Math("ADD", ior, one) iferr_return;
			const Term ratio = Math("DIVIDE", below, above) iferr_return;
			f0 = Math("MULTIPLY", ratio, ratio) iferr_return;
		}
		else
		{
			const Term curve = Input(rs, "user_curve", false, what, depth) iferr_return;
			f = Math("POWER", Term::Of(NodeOutput { layer, "Facing"_s }), curve) iferr_return;
			corrected = false;
		}
		Term facingColor = facing;
		if (corrected)
		{
			if (facing.IsConstant())
			{
				const maxon::Float64 largest = maxon::Max(facing.value.x, maxon::Max(facing.value.y, facing.value.z));
				facingColor = Term::Of(largest > 0.0 ? facing.value / largest : cinema::Vector(0.0));
			}
			else
			{
				const Term r = Component(facing, 0) iferr_return;
				const Term g = Component(facing, 1) iferr_return;
				const Term b = Component(facing, 2) iferr_return;
				const Term rg = Math("MAXIMUM", r, g) iferr_return;
				const Term largest = Math("MAXIMUM", rg, b) iferr_return;
				const Term largestVector = Combine(largest, largest, largest) iferr_return;
				facingColor = VMath("DIVIDE", facing, largestVector) iferr_return;
			}
			facingColor = VMath("SCALE", facingColor, f0) iferr_return;
		}
		const Term result = MixColor("MIX", f, facingColor, perpendicular) iferr_return;
		return Output(result, false);
	}

	// IOR To Metal Tints: the reflectance at normal incidence r = ((n - 1)^2 + k^2) / ((n + 1)^2 + k^2)
	// and Gulbrandsen's edge tint g = (nmax - n) / (nmax - nmin), nmin = (1 - r) / (1 + r),
	// nmax = (1 + sqrt r) / (1 - sqrt r), per channel (measured to 0.0004).
	if (name == "iortometaltints"_s)
	{
		const Term n = Input(rs, "ior", false, what, depth) iferr_return;
		const Term k = Input(rs, "k", false, what, depth) iferr_return;
		const Term ones = Term::Of(cinema::Vector(1.0));
		const Term below = VMath("SUBTRACT", n, ones) iferr_return;
		const Term above = VMath("ADD", n, ones) iferr_return;
		const Term k2 = VMath("MULTIPLY", k, k) iferr_return;
		const Term top = VMath("MULTIPLY_ADD", below, below, k2) iferr_return;
		const Term bottom = VMath("MULTIPLY_ADD", above, above, k2) iferr_return;
		const Term r = VMath("DIVIDE", top, bottom) iferr_return;
		if (source.output == "facing"_s)
			return Output(r, false);
		const Term root = VMath("POWER", r, Term::Of(cinema::Vector(0.5))) iferr_return;
		const Term minTop = VMath("SUBTRACT", ones, r) iferr_return;
		const Term minBottom = VMath("ADD", ones, r) iferr_return;
		const Term nMin = VMath("DIVIDE", minTop, minBottom) iferr_return;
		const Term maxTop = VMath("ADD", ones, root) iferr_return;
		const Term maxBottom = VMath("SUBTRACT", ones, root) iferr_return;
		const Term nMax = VMath("DIVIDE", maxTop, maxBottom) iferr_return;
		const Term gTop = VMath("SUBTRACT", nMax, n) iferr_return;
		const Term gBottom = VMath("SUBTRACT", nMax, nMin) iferr_return;
		const Term g = VMath("DIVIDE", gTop, gBottom) iferr_return;
		return Output(g, false);
	}

	// Surface Tangent: the tangent along the UV map ("From Mesh"), turned about the normal by
	// "rotation" turns towards the bitangent, or the user's vector as it is (measured).
	if (name == "surfacetangent"_s)
	{
		const maxon::Float64 from = Value(rs, "source", 1.0) iferr_return;
		if (from == 0.0)
		{
			const Term user = Input(rs, "usertangent", false, what, depth) iferr_return;
			return Output(user, false);
		}
		const maxon::Float64 turns = Setting(rs, "rotation", 0.0, what, " Surface Tangent") iferr_return;
		Term tangent = UvTangent() iferr_return;
		const maxon::Float64 angle = (turns - maxon::Floor(turns)) * maxon::PI2;
		if (maxon::Abs(angle) > 1e-9)
		{
			const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
			const Term bitangent = VMath("CROSS_PRODUCT", Term::Of(NodeOutput { geometry, "Normal"_s }), tangent) iferr_return;
			const Term along = VMath("SCALE", tangent, Term::Number(maxon::Cos(angle))) iferr_return;
			const Term across = VMath("SCALE", bitangent, Term::Number(maxon::Sin(angle))) iferr_return;
			tangent = VMath("ADD", along, across) iferr_return;
		}
		const Term result = ToCinema(tangent, false, 0) iferr_return;
		return Output(result, false);
	}

	// State: what Blender's Geometry, Texture Coordinate and Light Path nodes give, in Cinema 4D's
	// axes and units (positions and lengths in document units, measured), in world, object or camera
	// space.
	if (name == "state"_s)
	{
		const maxon::Float64 spaceValue = Value(rs, "trans_space", 0.0) iferr_return;
		maxon::Int space = maxon::Int(spaceValue);
		if (space == 3)
		{
			Warn(FormatString("a State node plugged into @ uses a reference object's space, which is not translated; world space is used.", maxon::String(what)));
			space = 0;
		}
		const maxon::String& output = source.output;
		const maxon::Float64 k = 1.0 / Coords::FromDocument(_exporter.GetDocument()).GetScale();
		if (output == "uvwcoord"_s)
		{
			const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
			return NodeOutput { coordinates, "UV"_s };
		}
		if (output == "raylength"_s || output == "raybouncelevel"_s)
		{
			const maxon::Int path = _graph.Add("ShaderNodeLightPath") iferr_return;
			if (output == "raylength"_s)
			{
				const Term length = Math("MULTIPLY", Term::Of(NodeOutput { path, "Ray Length"_s }), Term::Number(k)) iferr_return;
				return Output(length, true);
			}
			// All, Refraction, Reflection, Diffuse, Scatter, Transparency.
			const maxon::Float64 kind = Value(rs, "bounceleveltype", 0.0) iferr_return;
			static const maxon::Char* const DEPTHS[] = { "Ray Depth", "Transmission Depth", "Glossy Depth", "Diffuse Depth", "Ray Depth", "Transparent Depth" };
			const maxon::Int index = maxon::ClampValue(maxon::Int(kind), maxon::Int(0), maxon::Int(5));
			if (index == 4)
				Warn(FormatString("a State node plugged into @ counts volume scattering bounces, which Blender does not count apart; all bounces are counted.", maxon::String(what)));
			return NodeOutput { path, maxon::String(DEPTHS[index]) };
		}
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		Term vector;
		maxon::Bool point = false;
		if (output == "normal"_s || output == "normal_nobump"_s)
		{
			vector = Term::Of(NodeOutput { geometry, "Normal"_s });
		}
		else if (output == "normal_geom"_s)
		{
			vector = Term::Of(NodeOutput { geometry, "True Normal"_s });
		}
		else if (output == "tangent"_s || output == "bitangent"_s)
		{
			vector = UvTangent() iferr_return;
			if (output == "bitangent"_s)
			{
				vector = VMath("CROSS_PRODUCT", Term::Of(NodeOutput { geometry, "Normal"_s }), vector) iferr_return;
			}
		}
		else if (output == "raydirection"_s)
		{
			vector = VMath("SCALE", Term::Of(NodeOutput { geometry, "Incoming"_s }), Term::Number(-1.0)) iferr_return;
		}
		else if (output == "rayposition"_s)
		{
			vector = Term::Of(NodeOutput { geometry, "Position"_s });
			point = true;
		}
		else if (output == "rayorigin"_s)
		{
			// The point the ray left: position + incoming * ray length.
			const maxon::Int path = _graph.Add("ShaderNodeLightPath") iferr_return;
			const Term length = Term::Of(NodeOutput { path, "Ray Length"_s });
			const Term lengths = Combine(length, length, length) iferr_return;
			vector = VMath("MULTIPLY_ADD", Term::Of(NodeOutput { geometry, "Incoming"_s }), lengths, Term::Of(NodeOutput { geometry, "Position"_s })) iferr_return;
			point = true;
		}
		else
		{
			Warn(FormatString("the \"@\" output of a State node plugged into @ is not translated; the value set on the input is used.", output, maxon::String(what)));
			return NodeOutput();
		}
		const Term result = ToCinema(vector, point, space) iferr_return;
		return Output(result, false);
	}

	// (The Vertex Attribute node is with the other nodes that read the scene: redshift_scene_data.cpp.)

	// Ray Switch: the camera colour (its back colour on back faces when switched so), replaced for
	// reflection (glossy), refraction (transmission) and GI (diffuse) rays when their switch is on.
	if (name == "rayswitch"_s)
	{
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		const maxon::Int path = _graph.Add("ShaderNodeLightPath") iferr_return;
		const Term back = Term::Of(NodeOutput { geometry, "Backfacing"_s });
		auto sided = [&](const maxon::Char* front, const maxon::Char* backColor, const maxon::Char* switchPort) -> maxon::Result<Term>
		{
			iferr_scope;
			const Term color = Input(rs, front, colorData, what, depth) iferr_return;
			const maxon::Float64 twoSided = Value(rs, switchPort, 0.0) iferr_return;
			if (twoSided == 0.0)
				return color;
			const Term other = Input(rs, backColor, colorData, what, depth) iferr_return;
			return MixColor("MIX", back, color, other);
		};
		Term result = sided("cameracolor", "cameracolorback", "cameraswitchfrontback") iferr_return;
		struct Kind { const maxon::Char* on; const maxon::Char* color; const maxon::Char* back; const maxon::Char* twoSided; const maxon::Char* ray; };
		static const Kind KINDS[] =
		{
			{ "giswitch", "gicolor", "gicolorback", "giswitchfrontback", "Is Diffuse Ray" },
			{ "reflectionswitch", "reflectioncolor", "reflectioncolorback", "reflectionswitchfrontback", "Is Glossy Ray" },
			{ "refractionswitch", "refractioncolor", "refractioncolorback", "refractionswitchfrontback", "Is Transmission Ray" },
		};
		for (const Kind& kind : KINDS)
		{
			const maxon::Float64 on = Value(rs, kind.on, 0.0) iferr_return;
			if (on == 0.0)
				continue;
			const Term color = sided(kind.color, kind.back, kind.twoSided) iferr_return;
			result = MixColor("MIX", Term::Of(NodeOutput { path, maxon::String(kind.ray) }), result, color) iferr_return;
		}
		return Output(result, false);
	}

	// WireFrame: Blender's Wireframe node in pixels, mixing the polygon and the wire colour.
	if (name == "wireframe"_s)
	{
		const maxon::Float64 thickness = Setting(rs, "thickness", 0.5, what, " WireFrame") iferr_return;
		const maxon::Float64 hidden = Value(rs, "showhiddenedges", 1.0) iferr_return;
		if (hidden == 0.0)
			Warn(FormatString("a WireFrame node plugged into @ hides the edges inside polygons, which Blender's Wireframe node shows (it draws every triangle).", maxon::String(what)));
		const maxon::Int wire = _graph.Add("ShaderNodeWireframe") iferr_return;
		_graph.SetProperty(wire, "use_pixel_size", true);
		_graph.SetInput(wire, "Size", thickness * WIRE_PIXELS);
		const Term polygon = Input(rs, "polycolor", colorData, what, depth) iferr_return;
		const Term edge = Input(rs, "wirecolor", colorData, what, depth) iferr_return;
		const Term result = MixColor("MIX", Term::Of(NodeOutput { wire, "Fac"_s }), polygon, edge) iferr_return;
		return Output(result, false);
	}

	// AO: Blender's Ambient Occlusion node between the dark and the bright colour. Redshift's spread
	// and falloff shape its cone of samples, which Blender's cosine-weighted hemisphere lacks; only
	// the distance (0: unlimited) and "same object only" are taken.
	if (name == "ambientocclusion"_s)
	{
		const maxon::Float64 mode = Value(rs, "outputmode", 0.0) iferr_return;
		if (mode != 0.0)
		{
			Warn(FormatString("an AO node plugged into @ returns the environment or bent normals, which is not translated; the value set on the input is used.", maxon::String(what)));
			return NodeOutput();
		}
		const maxon::Float64 distance = Setting(rs, "maxdistance", 0.0, what, "n AO") iferr_return;
		const maxon::Float64 local = Value(rs, "sameobjectonly", 0.0) iferr_return;
		const maxon::Float64 invert = Value(rs, "invert", 0.0) iferr_return;
		const maxon::Float64 spread = Value(rs, "spread", 0.8) iferr_return;
		const maxon::Float64 falloff = Value(rs, "falloff", 1.0) iferr_return;
		if (spread != 0.8 || falloff != 1.0)
			Warn(FormatString("the spread and falloff of an AO node plugged into @ are not translated (Blender samples the whole hemisphere).", maxon::String(what)));
		const maxon::Int ao = _graph.Add("ShaderNodeAmbientOcclusion") iferr_return;
		_graph.SetProperty(ao, "only_local", local != 0.0);
		_graph.SetInput(ao, "Distance", distance > 0.0 ? Coords::FromDocument(_exporter.GetDocument()).Length(distance) : 1000.0);
		const Term bright = Input(rs, "bright", colorData, what, depth) iferr_return;
		const Term dark = Input(rs, "dark", colorData, what, depth) iferr_return;
		const Term result = (invert != 0.0 ? MixColor("MIX", Term::Of(NodeOutput { ao, "AO"_s }), bright, dark)
			: MixColor("MIX", Term::Of(NodeOutput { ao, "AO"_s }), dark, bright)) iferr_return;
		return Output(result, false);
	}

	handled = false;
	return NodeOutput();
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::Projected(const maxon::GraphNode& rsNode, const maxon::Char* port, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, const Term& vector, const maxon::Char* label)
{
	iferr_scope;
	const maxon::Id asset = AssetOf(rsNode) iferr_return;
	const maxon::GraphNode input = InputPort(rsNode, asset.ToString(), port) iferr_return;
	const Source source = Find(input) iferr_return;
	if (source.node.IsValid())
	{
		const maxon::String wiredName = ShortName(source.node) iferr_return;
		if (wiredName == "texturesampler"_s)
		{
			const NodeOutput sampled = TextureSampler(source.node, colorData, what, &vector) iferr_return;
			if (sampled.IsValid())
				return Term::Of(sampled);
		}
		else
		{
			Warn(FormatString("the \"@\" node plugged into the @ of a@ node (in @) is not a Texture node, so it is not projected; it keeps its own coordinates.",
				wiredName, maxon::String(port), maxon::String(label), maxon::String(what)));
		}
	}
	return Input(rsNode, port, colorData, what, depth);
}

// The position along a Ramp (measured): the source (UV map, or the alt input when wired), the
// mapping of U and V (vertical V, horizontal U, diagonal (U + V) / 2, radial: the angle round the
// centre from below through left, up and right, circular: the distance from the centre times
// sqrt 2), then times 2^exposure, the old range to the new, held to 0..1, inverted.
maxon::Result<RedshiftNodes::Term> RedshiftNodes::RampPosition(const maxon::GraphNode& rs, const maxon::String& asset, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	const maxon::Char* const label = asset == "rsramp"_s ? " Ramp" : " Scalar Ramp";
	const maxon::GraphNode altPort = InputPort(rs, maxon::String(RS_NODE) + asset, "input") iferr_return;
	const Source alt = Find(altPort) iferr_return;
	const maxon::Float64 source = Value(rs, "inputsource", 2.0) iferr_return;
	const maxon::GraphNode contextPort = InputPort(rs, maxon::String(RS_NODE) + asset, "uv_context") iferr_return;
	const Source context = Find(contextPort) iferr_return;
	if (context.node.IsValid())
		Warn(FormatString("the UV context wired into a@ node plugged into @ is not translated; the object's UV map is used.", maxon::String(label), maxon::String(what)));
	const maxon::Float64 noise = Value(rs, "noise", 0.0) iferr_return;
	if (noise != 0.0)
		Warn(FormatString("the noise of a@ node plugged into @ is not translated.", maxon::String(label), maxon::String(what)));
	Term t;
	if (source == 1.0 || (source == 2.0 && alt.node.IsValid()))
	{
		t = Input(rs, "input", false, what, depth) iferr_return;
	}
	else
	{
		// The UV coordinates as the texture tag places them (measured, F13 W18: a Ramp repeats with the tag's tiles).
		const Term uv = MaterialUv() iferr_return;
		const Term u = Component(uv, 0) iferr_return;
		const Term v = Component(uv, 1) iferr_return;
		const maxon::Float64 mapping = Value(rs, "inputmapping", 0.0) iferr_return;
		if (mapping == 1.0)
		{
			t = u;
		}
		else if (mapping == 2.0)
		{
			const Term sum = Math("ADD", u, v) iferr_return;
			t = Math("MULTIPLY", sum, Term::Number(0.5)) iferr_return;
		}
		else if (mapping == 3.0)
		{
			const Term x = Math("SUBTRACT", Term::Number(0.5), u) iferr_return;
			const Term y = Math("SUBTRACT", Term::Number(0.5), v) iferr_return;
			const Term angle = Math("ARCTAN2", x, y) iferr_return;
			const Term turns = Math("MULTIPLY", angle, Term::Number(1.0 / maxon::PI2)) iferr_return;
			t = Math("FRACT", turns) iferr_return;
		}
		else if (mapping == 4.0)
		{
			const Term centred = VMath("SUBTRACT", uv, Term::Of(cinema::Vector(0.5, 0.5, 0.0))) iferr_return;
			const Term distance = VMath("LENGTH", centred) iferr_return;
			t = Math("MULTIPLY", distance, Term::Number(maxon::Sqrt(2.0))) iferr_return;
		}
		else
		{
			t = v;
		}
	}
	const maxon::Float64 exposure = Setting(rs, "exposure", 0.0, what, label) iferr_return;
	if (exposure != 0.0)
	{
		t = Math("MULTIPLY", t, Term::Number(maxon::Pow(2.0, exposure))) iferr_return;
	}
	const maxon::Float64 oldMin = Setting(rs, "old_min", 0.0, what, label) iferr_return;
	const maxon::Float64 oldMax = Setting(rs, "old_max", 1.0, what, label) iferr_return;
	const maxon::Float64 newMin = Setting(rs, "new_min", 0.0, what, label) iferr_return;
	const maxon::Float64 newMax = Setting(rs, "new_max", 1.0, what, label) iferr_return;
	if (oldMin != 0.0 || oldMax != 1.0 || newMin != 0.0 || newMax != 1.0)
	{
		const maxon::Float64 span = oldMax - oldMin;
		const maxon::Float64 k = span != 0.0 ? (newMax - newMin) / span : 0.0;
		t = Math("MULTIPLY_ADD", t, Term::Number(k), Term::Number(newMin - oldMin * k)) iferr_return;
	}
	// Held to 0..1 (a Math node's clamp).
	const maxon::Int held = _graph.Add("ShaderNodeMath") iferr_return;
	_graph.SetProperty(held, "operation", "ADD"_s);
	_graph.SetProperty(held, "use_clamp", true);
	Set(held, "Value", t, true) iferr_return;
	_graph.SetInput(held, "Value_001", 0.0);
	t = Term::Of(NodeOutput { held, "Value"_s });
	const maxon::Float64 invert = Value(rs, "inputinvert", 0.0) iferr_return;
	if (invert != 0.0)
	{
		t = Math("SUBTRACT", Term::Number(1.0), t) iferr_return;
	}
	return t;
}

// A knot of a Ramp's gradient.
struct RampKnot
{
	maxon::Float64 position = 0.0;
	cinema::Vector color;
	maxon::String interpolation;
	maxon::Float64 bias = 0.5;
};

// The colour of the gradient at t (measured): within a segment the position is first moved by
// the left knot's bias (bias b takes the half value: 0.5 t / b below b, 0.5 + 0.5 (t - b) / (1 - b)
// above), then the left knot's interpolation applies: smooth (smoothstep), linear, step (none), blend
// (linear from the bias on, instead of the move), cubic (Catmull and Rom, the end knots repeated).
static cinema::Vector EvaluateRamp(const maxon::BaseArray<RampKnot>& knots, maxon::Float64 t)
{
	const maxon::Int count = knots.GetCount();
	if (t <= knots[0].position)
		return knots[0].color;
	if (t >= knots[count - 1].position)
		return knots[count - 1].color;
	maxon::Int i = 0;
	while (i < count - 2 && t >= knots[i + 1].position)
		++i;
	const RampKnot& a = knots[i];
	const RampKnot& b = knots[i + 1];
	const maxon::Float64 width = b.position - a.position;
	maxon::Float64 f = width > 0.0 ? (t - a.position) / width : 1.0;
	const maxon::Float64 bias = maxon::ClampValue(a.bias, 1e-4, 1.0 - 1e-4);
	if (a.interpolation == "blend"_s)
	{
		f = f < bias ? 0.0 : (f - bias) / (1.0 - bias);
		return a.color + (b.color - a.color) * f;
	}
	if (a.bias != 0.5)
		f = f < bias ? 0.5 * f / bias : 0.5 + 0.5 * (f - bias) / (1.0 - bias);
	if (a.interpolation == "none"_s)
		return a.color;
	if (a.interpolation == "linearknot"_s)
		return a.color + (b.color - a.color) * f;
	if (a.interpolation == "cubicknot"_s || a.interpolation == "cubicbias"_s)
	{
		const cinema::Vector& before = i > 0 ? knots[i - 1].color : a.color;
		const cinema::Vector& after = i + 2 < count ? knots[i + 2].color : b.color;
		const maxon::Float64 f2 = f * f, f3 = f2 * f;
		return (a.color * 2.0 + (b.color - before) * f + (before * 2.0 - a.color * 5.0 + b.color * 4.0 - after) * f2
			+ (a.color * 3.0 - before - b.color * 3.0 + after) * f3) * 0.5;
	}
	const maxon::Float64 smooth = f * f * (3.0 - 2.0 * f);
	return a.color + (b.color - a.color) * smooth;
}

// The children of a variadic bundle port, in their order.
static maxon::Result<maxon::BaseArray<maxon::GraphNode>> Children(const maxon::GraphNode& port)
{
	iferr_scope;
	maxon::BaseArray<maxon::GraphNode> found;
	if (!port.IsValid())
		return found;
	port.GetChildren([&found](const maxon::GraphNode& child) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			found.Append(child) iferr_return;
			return true;
		}, maxon::NODE_KIND::INPORT) iferr_return;
	return found;
}

static maxon::Float64 ChildNumber(const maxon::GraphNode& bundle, const maxon::Char* name, maxon::Float64 fallback)
{
	maxon::GraphNode port;
	iferr (port = bundle.FindChild(maxon::Id(name)))
		return fallback;
	const PortValue value = ReadValue(port);
	return value.found && !value.isVector ? value.number : fallback;
}

static cinema::Vector ChildVector(const maxon::GraphNode& bundle, const maxon::Char* name, const cinema::Vector& fallback, maxon::Bool* found = nullptr)
{
	maxon::GraphNode port;
	iferr (port = bundle.FindChild(maxon::Id(name)))
		return fallback;
	const PortValue value = ReadValue(port);
	if (found)
		*found = value.found && value.isVector;
	return value.found && value.isVector ? value.vector : fallback;
}

static maxon::String ChildId(const maxon::GraphNode& bundle, const maxon::Char* name, const maxon::String& fallback)
{
	maxon::GraphNode port;
	iferr (port = bundle.FindChild(maxon::Id(name)))
		return fallback;
	if (!port.IsValid())
		return fallback;
	ifnoerr (const maxon::Opt<const maxon::InternedId&> id = port.GetEffectivePortValue<maxon::InternedId>())
	{
		if (id)
			return id->ToString();
	}
	ifnoerr (const maxon::Opt<const maxon::Id&> plain = port.GetEffectivePortValue<maxon::Id>())
	{
		if (plain)
			return plain->ToString();
	}
	return fallback;
}

// A gradient bundle (the Ramp's "ramp", the Brick's colours): smooth, linear and step gradients
// without bias are Blender's Ease, Linear and Constant ramps exactly; any other is the gradient
// evaluated as Redshift does (measured) at 32 positions joined by straight lines.
maxon::Result<NodeOutput> RedshiftNodes::Gradient(const maxon::GraphNode& port, const Term& t, const maxon::Char* what, const maxon::Char* label)
{
	iferr_scope;
	const maxon::BaseArray<maxon::GraphNode> children = Children(port) iferr_return;
	maxon::BaseArray<RampKnot> knots;
	for (const maxon::GraphNode& child : children)
	{
		RampKnot knot;
		knot.position = ChildNumber(child, "position", 0.0);
		knot.color = ChildVector(child, "color", cinema::Vector(0.0));
		knot.interpolation = ChildId(child, "interpolation", "smoothknot"_s);
		knot.bias = ChildNumber(child, "bias", 0.5);
		knots.Append(knot) iferr_return;
	}
	if (knots.IsEmpty())
	{
		Warn(FormatString("a@ node plugged into @ has no knots; the value set on the input is used.", maxon::String(label), maxon::String(what)));
		return NodeOutput();
	}
	for (maxon::Int i = 1; i < knots.GetCount(); ++i)
	{
		for (maxon::Int j = i; j > 0 && knots[j].position < knots[j - 1].position; --j)
			std::swap(knots[j], knots[j - 1]);
	}
	maxon::String kind = knots[0].interpolation;
	maxon::Bool plain = true;
	for (const RampKnot& knot : knots)
	{
		if (knot.interpolation != kind || knot.bias != 0.5)
			plain = false;
	}
	const maxon::Char* blenderKind = kind == "smoothknot"_s ? "EASE" : kind == "linearknot"_s ? "LINEAR" : kind == "none"_s ? "CONSTANT" : nullptr;
	if (!blenderKind || knots.GetCount() > 32)
		plain = false;
	maxon::String elements;
	auto element = [&elements](maxon::Float64 position, const cinema::Vector& color)
	{
		elements += FormatString("@[@, @, @, @, 1.0]", elements.IsEmpty() ? ""_s : ", "_s, position, color.x, color.y, color.z);
	};
	if (plain)
	{
		for (const RampKnot& knot : knots)
			element(knot.position, knot.color);
	}
	else
	{
		blenderKind = "LINEAR";
		const maxon::Float64 first = knots[0].position;
		const maxon::Float64 last = knots[knots.GetCount() - 1].position;
		const maxon::Int steps = last > first ? 31 : 0;
		for (maxon::Int step = 0; step <= steps; ++step)
		{
			const maxon::Float64 position = steps ? first + (last - first) * maxon::Float64(step) / maxon::Float64(steps) : first;
			element(position, EvaluateRamp(knots, position));
		}
	}
	const maxon::Int ramp = _graph.Add("ShaderNodeValToRGB") iferr_return;
	_graph.SetPropertyJson(ramp, "color_ramp", FormatString("{\"interpolation\": \"@\", \"elements\": [@]}", maxon::String(blenderKind), elements));
	Set(ramp, "Fac", t, true) iferr_return;
	return NodeOutput { ramp, "Color"_s };
}

// The children of a variadic bundle port, for redshift_lookalikes.cpp.
maxon::Result<maxon::BaseArray<maxon::GraphNode>> RedshiftNodes::Entries(const maxon::GraphNode& port)
{
	return Children(port);
}

//----------------------------------------------------------------------------------------
// Texture nodes: Ramp, Scalar Ramp, Camera Map, MatCap, UV Projection, TriPlanar.
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::TextureNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;

	// Ramp: a Color Ramp at the ramp's position. Smooth, linear and step gradients without bias are
	// Blender's Ease, Linear and Constant ramps exactly; any other is the gradient evaluated as
	// Redshift does (measured) at 32 positions joined by straight lines. The node's "Interpolation
	// Type" changes nothing in Redshift (measured): each knot's interpolation counts.
	if (name == "rsramp"_s)
	{
		const maxon::GraphNode rampPort = InputPort(rs, maxon::String(RS_NODE) + name, "ramp") iferr_return;
		const Term t = RampPosition(rs, name, what, depth) iferr_return;
		return Gradient(rampPort, t, what, " Ramp");
	}

	// Scalar Ramp: a Float Curve at the ramp's position. Linear points are exact; spline points
	// follow their tangents as a cubic Bezier segment, cubic points (and spline points without
	// tangents) a Catmull-Rom segment; sampled in 16 steps per segment.
	if (name == "rsscalarramp"_s)
	{
		const maxon::GraphNode rampPort = InputPort(rs, maxon::String(RS_NODE) + name, "ramp") iferr_return;
		maxon::GraphNode pointsPort;
		if (rampPort.IsValid())
		{
			pointsPort = rampPort.FindChild(maxon::Id("points")) iferr_return;
		}
		const maxon::BaseArray<maxon::GraphNode> children = Children(pointsPort) iferr_return;
		struct Point { maxon::Float64 x, y; maxon::String kind; cinema::Vector left, right; maxon::Bool tangents; };
		maxon::BaseArray<Point> points;
		for (const maxon::GraphNode& child : children)
		{
			Point point;
			point.x = ChildNumber(child, "positionx", 0.0);
			point.y = ChildNumber(child, "positiony", 0.0);
			point.kind = ChildId(child, "interpolation", "spline"_s);
			maxon::Bool hasLeft = false, hasRight = false;
			point.left = ChildVector(child, "tangentleft", cinema::Vector(0.0), &hasLeft);
			point.right = ChildVector(child, "tangentright", cinema::Vector(0.0), &hasRight);
			point.tangents = hasLeft && hasRight;
			points.Append(point) iferr_return;
		}
		if (points.IsEmpty())
		{
			Warn(FormatString("a Scalar Ramp node plugged into @ has no points; the value set on the input is used.", maxon::String(what)));
			return NodeOutput();
		}
		for (maxon::Int i = 1; i < points.GetCount(); ++i)
		{
			for (maxon::Int j = i; j > 0 && points[j].x < points[j - 1].x; --j)
				std::swap(points[j], points[j - 1]);
		}
		maxon::String curve;
		maxon::Float64 low = 0.0, high = 1.0, left = 0.0, right = 1.0;
		auto add = [&](maxon::Float64 x, maxon::Float64 y)
		{
			curve += FormatString("@[@, @, \"VECTOR\"]", curve.IsEmpty() ? ""_s : ", "_s, x, y);
			low = maxon::Min(low, y);
			high = maxon::Max(high, y);
			left = maxon::Min(left, x);
			right = maxon::Max(right, x);
		};
		const maxon::Int count = points.GetCount();
		add(points[0].x, points[0].y);
		for (maxon::Int i = 0; i + 1 < count; ++i)
		{
			const Point& a = points[i];
			const Point& b = points[i + 1];
			if (a.kind == "linear"_s)
			{
				add(b.x, b.y);
				continue;
			}
			const maxon::Bool bezier = a.kind == "spline"_s && a.tangents && b.tangents;
			const Point& before = i > 0 ? points[i - 1] : a;
			const Point& after = i + 2 < count ? points[i + 2] : b;
			for (maxon::Int step = 1; step <= 16; ++step)
			{
				const maxon::Float64 f = maxon::Float64(step) / 16.0;
				if (bezier)
				{
					const maxon::Float64 g = 1.0 - f;
					const maxon::Float64 x = g * g * g * a.x + 3.0 * g * g * f * (a.x + a.right.x) + 3.0 * g * f * f * (b.x + b.left.x) + f * f * f * b.x;
					const maxon::Float64 y = g * g * g * a.y + 3.0 * g * g * f * (a.y + a.right.y) + 3.0 * g * f * f * (b.y + b.left.y) + f * f * f * b.y;
					add(maxon::ClampValue(x, a.x, b.x), y);
				}
				else
				{
					const maxon::Float64 f2 = f * f, f3 = f2 * f;
					const maxon::Float64 y = 0.5 * (2.0 * a.y + (b.y - before.y) * f + (2.0 * before.y - 5.0 * a.y + 4.0 * b.y - after.y) * f2
						+ (3.0 * a.y - before.y - 3.0 * b.y + after.y) * f3);
					add(a.x + (b.x - a.x) * f, y);
				}
			}
		}
		if (count == 1)
			add(points[0].x + 1e-3, points[0].y);
		const Term t = RampPosition(rs, name, what, depth) iferr_return;
		const maxon::Int node = _graph.Add("ShaderNodeFloatCurve") iferr_return;
		_graph.SetPropertyJson(node, "mapping", FormatString("{\"extend\": \"HORIZONTAL\", \"clip\": [@, @, @, @], \"curves\": [[@]]}",
			maxon::Max(-100.0, left), maxon::Max(-100.0, low), maxon::Min(100.0, right), maxon::Min(100.0, high), curve));
		_graph.SetInput(node, "Factor", 1.0);
		Set(node, "Value", t, true) iferr_return;
		return NodeOutput { node, "Value"_s };
	}

	// Camera Map: its own file seen through the render camera: Blender's window coordinates. With
	// "Texture" aspect the picture keeps its own proportions and fills the frame (measured with a
	// square file in a 2:1 frame: as wide as the frame, cropped above and below); the frame's
	// proportions are the document's render resolution, which the IPR's "Fit Window" changes.
	if (name == "cameramap"_s)
	{
		const maxon::String image = FileImage(rs, name, colorData, what, "Camera Map") iferr_return;
		if (image.IsEmpty())
			return NodeOutput();
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		Term uv = Term::Of(NodeOutput { coordinates, "Window"_s });
		const maxon::Float64 aspect = Value(rs, "backplateaspect", 0.0) iferr_return;
		if (aspect == 0.0)
		{
			maxon::Float64 frame = 16.0 / 9.0;
			const cinema::RenderData* const renderData = _exporter.GetDocument() ? _exporter.GetDocument()->GetActiveRenderData() : nullptr;
			if (renderData)
			{
				const cinema::BaseContainer& data = renderData->GetDataInstanceRef();
				frame = maxon::Float64(maxon::Max(maxon::Int32(1), data.GetInt32(RDATA_XRES, 1280))) / maxon::Float64(maxon::Max(maxon::Int32(1), data.GetInt32(RDATA_YRES, 720)))
					* data.GetFloat(RDATA_PIXELASPECT, 1.0);
			}
			// The file's own proportions, which only Blender knows: width / height = texel height / texel width.
			const maxon::Int ratio = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(ratio, "operation", "DIVIDE"_s);
			_graph.SetInputJson(ratio, "Value", FormatString("{\"image\":\"@\",\"is\":\"texel_height\"}", image));
			_graph.SetInputJson(ratio, "Value_001", FormatString("{\"image\":\"@\",\"is\":\"texel_width\"}", image));
			const Term picture = Term::Of(NodeOutput { ratio, "Value"_s });
			// Scale about the centre: u by min(1, frame / picture), v by min(1, picture / frame).
			const Term uScale = Math("DIVIDE", Term::Number(frame), picture) iferr_return;
			const Term uHeld = Math("MINIMUM", uScale, Term::Number(1.0)) iferr_return;
			const Term vScale = Math("DIVIDE", picture, Term::Number(frame)) iferr_return;
			const Term vHeld = Math("MINIMUM", vScale, Term::Number(1.0)) iferr_return;
			const Term u = Component(uv, 0) iferr_return;
			const Term v = Component(uv, 1) iferr_return;
			const Term uCentred = Math("SUBTRACT", u, Term::Number(0.5)) iferr_return;
			const Term vCentred = Math("SUBTRACT", v, Term::Number(0.5)) iferr_return;
			const Term uScaled = Math("MULTIPLY_ADD", uCentred, uHeld, Term::Number(0.5)) iferr_return;
			const Term vScaled = Math("MULTIPLY_ADD", vCentred, vHeld, Term::Number(0.5)) iferr_return;
			uv = Combine(uScaled, vScaled, Term::Number(0.0)) iferr_return;
		}
		const maxon::Float64 gamma = Setting(rs, "tex0_gamma", 1.0, what, " Camera Map") iferr_return;
		const maxon::Int texture = _graph.Add("ShaderNodeTexImage") iferr_return;
		_graph.SetProperty(texture, "image", image);
		_graph.SetProperty(texture, "extension", "CLIP"_s);
		Set(texture, "Vector", uv, false) iferr_return;
		Term color = Term::Of(NodeOutput { texture, "Color"_s });
		if (gamma != 1.0)
		{
			const maxon::Int correct = _graph.Add("ShaderNodeGamma") iferr_return;
			_graph.SetInput(correct, "Gamma", gamma);
			Set(correct, "Color", color, false) iferr_return;
			color = Term::Of(NodeOutput { correct, "Color"_s });
		}
		return Output(color, false);
	}

	// MatCap: the wired texture at the normal's x and y in camera (or world) space mapped to 0..1,
	// scaled and turned about the centre.
	if (name == "matcap"_s)
	{
		const maxon::Float64 space = Value(rs, "space", 1.0) iferr_return;
		if (space == 2.0)
			Warn(FormatString("a MatCap node plugged into @ uses the \"Normal\" space, which is not translated; camera space is used.", maxon::String(what)));
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		Term normal;
		if (space == 0.0)
		{
			// "World" measures the normal against the ray (measured: the middle of a ball off the
			// view's axis reads 0.5): x along up x incoming, y along incoming x that, up being world up.
			const Term incoming = Term::Of(NodeOutput { geometry, "Incoming"_s });
			const Term n = Term::Of(NodeOutput { geometry, "Normal"_s });
			const Term across = VMath("CROSS_PRODUCT", Term::Of(cinema::Vector(0.0, 0.0, 1.0)), incoming) iferr_return;
			const Term right = VMath("NORMALIZE", across) iferr_return;
			const Term up = VMath("CROSS_PRODUCT", incoming, right) iferr_return;
			const Term x = VMath("DOT_PRODUCT", n, right) iferr_return;
			const Term y = VMath("DOT_PRODUCT", n, up) iferr_return;
			normal = Combine(x, y, Term::Number(0.0)) iferr_return;
		}
		else
		{
			normal = ToCinema(Term::Of(NodeOutput { geometry, "Normal"_s }), false, 2) iferr_return;
		}
		const maxon::Float64 scaling = Setting(rs, "scaling", 1.0, what, " MatCap") iferr_return;
		const maxon::Float64 scaleU = Setting(rs, "scalingu", 1.0, what, " MatCap") iferr_return;
		const maxon::Float64 scaleV = Setting(rs, "scalingv", 1.0, what, " MatCap") iferr_return;
		const maxon::Float64 rotation = Setting(rs, "rotation", 0.0, what, " MatCap") iferr_return;
		const maxon::Int mapping = _graph.Add("ShaderNodeMapping") iferr_return;
		_graph.SetProperty(mapping, "vector_type", "POINT"_s);
		Set(mapping, "Vector", normal, false) iferr_return;
		_graph.SetInput(mapping, "Scale", cinema::Vector(0.5 * scaling * scaleU, 0.5 * scaling * scaleV, 0.0));
		_graph.SetInput(mapping, "Rotation", cinema::Vector(0.0, 0.0, maxon::DegToRad(rotation)));
		_graph.SetInput(mapping, "Location", cinema::Vector(0.5, 0.5, 0.0));
		const Term uv = Term::Of(NodeOutput { mapping, "Vector"_s });
		const Term color = Projected(rs, "color", colorData, what, depth, uv, " MatCap") iferr_return;
		return Output(color, false);
	}

	// UV Projection: the wired texture at coordinates projected from the object or the world.
	// Planar (measured): 0.5 + 0.5 x scale (x, y) of the position in Cinema 4D's axes and units.
	if (name == "uvprojection"_s)
	{
		const maxon::Float64 type = Value(rs, "proj_type", 1.0) iferr_return;
		const maxon::Float64 spaceValue = Value(rs, "coord_space", 0.0) iferr_return;
		maxon::Int space = maxon::Int(spaceValue);
		if (space == 2)
		{
			Warn(FormatString("a UV Projection node plugged into @ projects from a reference object, which is not translated; world space is used.", maxon::String(what)));
			space = 0;
		}
		const cinema::Vector scale = VectorValue(rs, "coord_scale", cinema::Vector(0.01)) iferr_return;
		const cinema::Vector rotate = VectorValue(rs, "coord_rotate", cinema::Vector(0.0)) iferr_return;
		const cinema::Vector offset = VectorValue(rs, "coord_offset", cinema::Vector(0.0)) iferr_return;
		const cinema::Vector uvScale = VectorValue(rs, "uv_scale", cinema::Vector(1.0, 1.0, 0.0)) iferr_return;
		const cinema::Vector uvOffset = VectorValue(rs, "uv_offset", cinema::Vector(0.0)) iferr_return;
		const maxon::Float64 uvRotate = Value(rs, "uv_rotate", 0.0) iferr_return;
		if (rotate != cinema::Vector(0.0))
			Warn(FormatString("the rotation of a UV Projection node plugged into @ is not translated.", maxon::String(what)));
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		Term p = ToCinema(Term::Of(NodeOutput { geometry, "Position"_s }), true, space) iferr_return;
		p = VMath("MULTIPLY", p, Term::Of(scale)) iferr_return;
		p = VMath("ADD", p, Term::Of(offset)) iferr_return;
		Term uv;
		if (type == 2.0 || type == 3.0 || type == 4.0)
		{
			// Longitude round the y axis, and latitude (spherical) or height (cylindrical).
			const maxon::Float64 angleU = Value(rs, "angle_u", 180.0) iferr_return;
			const maxon::Float64 angleV = Value(rs, "angle_v", 90.0) iferr_return;
			const Term x = Component(p, 0) iferr_return;
			const Term y = Component(p, 1) iferr_return;
			const Term z = Component(p, 2) iferr_return;
			// Measured: spherical u = 0.5 + atan2(x, z) / (2 angle), the seam in front (-z);
			// cylindrical u = 0.5 + atan2(-x, -z) / angle.
			Term u;
			if (type == 3.0)
			{
				const Term negX = Math("MULTIPLY", x, Term::Number(-1.0)) iferr_return;
				const Term negZ = Math("MULTIPLY", z, Term::Number(-1.0)) iferr_return;
				const Term longitude = Math("ARCTAN2", negX, negZ) iferr_return;
				u = Math("MULTIPLY_ADD", longitude, Term::Number(1.0 / maxon::DegToRad(angleU)), Term::Number(0.5)) iferr_return;
			}
			else
			{
				const Term longitude = Math("ARCTAN2", x, z) iferr_return;
				u = Math("MULTIPLY_ADD", longitude, Term::Number(0.5 / maxon::DegToRad(angleU)), Term::Number(0.5)) iferr_return;
			}
			Term v;
			if (type == 3.0)
			{
				v = Math("MULTIPLY_ADD", y, Term::Number(0.5), Term::Number(0.5)) iferr_return;
			}
			else
			{
				const Term length = VMath("LENGTH", p) iferr_return;
				const Term sine = Math("DIVIDE", y, length) iferr_return;
				const Term latitude = Math("ARCSINE", sine) iferr_return;
				v = Math("MULTIPLY_ADD", latitude, Term::Number(0.5 / maxon::DegToRad(angleV)), Term::Number(0.5)) iferr_return;
			}
			if (type == 4.0)
				Warn(FormatString("the \"Ball\" projection of a UV Projection node plugged into @ is translated as spherical.", maxon::String(what)));
			uv = Combine(u, v, Term::Number(0.0)) iferr_return;
		}
		else if (type == 5.0)
		{
			Warn(FormatString("the cubic projection of a UV Projection node plugged into @ is translated as planar.", maxon::String(what)));
			uv = VMath("MULTIPLY_ADD", p, Term::Of(cinema::Vector(0.5, 0.5, 0.0)), Term::Of(cinema::Vector(0.5, 0.5, 0.0))) iferr_return;
		}
		else
		{
			uv = VMath("MULTIPLY_ADD", p, Term::Of(cinema::Vector(0.5, 0.5, 0.0)), Term::Of(cinema::Vector(0.5, 0.5, 0.0))) iferr_return;
		}
		if (uvScale != cinema::Vector(1.0, 1.0, 0.0) || uvOffset != cinema::Vector(0.0) || uvRotate != 0.0)
		{
			const maxon::Int mapping = _graph.Add("ShaderNodeMapping") iferr_return;
			_graph.SetProperty(mapping, "vector_type", "POINT"_s);
			Set(mapping, "Vector", uv, false) iferr_return;
			_graph.SetInput(mapping, "Scale", cinema::Vector(uvScale.x, uvScale.y, 1.0));
			_graph.SetInput(mapping, "Location", cinema::Vector(uvOffset.x, uvOffset.y, 0.0));
			_graph.SetInput(mapping, "Rotation", cinema::Vector(0.0, 0.0, maxon::DegToRad(uvRotate)));
			uv = Term::Of(NodeOutput { mapping, "Vector"_s });
		}
		const Term color = Projected(rs, "color", colorData, what, depth, uv, " UV Projection") iferr_return;
		return Output(color, false);
	}

	// TriPlanar: the wired texture (or one per axis) seen along x, y and z of the object or the
	// world at (position + offset) x scale, in document units, blended by the normal: per axis
	// s = 0.5 + (|n_i| - max of the other two) / blend amount, held to 0..1, to the power of half
	// the blend curve, the three normalised (measured on spheres and planes).
	if (name == "triplanar"_s)
	{
		const maxon::Float64 spaceValue = Value(rs, "projspacetype", 1.0) iferr_return;
		maxon::Int space = maxon::Int(spaceValue);
		if (space == 2)
		{
			Warn(FormatString("a TriPlanar node plugged into @ projects from a reference object, which is not translated; object space is used.", maxon::String(what)));
			space = 1;
		}
		const cinema::Vector scale = VectorValue(rs, "scale", cinema::Vector(0.01)) iferr_return;
		const cinema::Vector offset = VectorValue(rs, "offset", cinema::Vector(0.0)) iferr_return;
		const cinema::Vector rotation = VectorValue(rs, "rotation", cinema::Vector(0.0)) iferr_return;
		if (rotation != cinema::Vector(0.0))
			Warn(FormatString("the rotation of a TriPlanar node plugged into @ is not translated.", maxon::String(what)));
		const maxon::Float64 blendAmount = Setting(rs, "blendamount", 0.1, what, " TriPlanar") iferr_return;
		const maxon::Float64 amount = maxon::Max(1e-3, blendAmount);
		const maxon::Float64 curve = Setting(rs, "blendcurve", 2.0, what, " TriPlanar") iferr_return;
		const maxon::Float64 sameValue = Value(rs, "sameimageoneachaxis", 1.0) iferr_return;
		const maxon::Bool same = sameValue != 0.0;
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		Term p = ToCinema(Term::Of(NodeOutput { geometry, "Position"_s }), true, space) iferr_return;
		p = VMath("ADD", p, Term::Of(offset)) iferr_return;
		p = VMath("MULTIPLY", p, Term::Of(scale)) iferr_return;
		const Term n = ToCinema(Term::Of(NodeOutput { geometry, "Normal"_s }), false, space) iferr_return;
		const Term a = VMath("ABSOLUTE", n) iferr_return;
		const Term ax = Component(a, 0) iferr_return;
		const Term ay = Component(a, 1) iferr_return;
		const Term az = Component(a, 2) iferr_return;
		const Term px = Component(p, 0) iferr_return;
		const Term py = Component(p, 1) iferr_return;
		const Term pz = Component(p, 2) iferr_return;
		// Coordinates per axis (u, v) and the weight.
		const Term* const axes[3] = { &ax, &ay, &az };
		Term weights[3];
		for (maxon::Int i = 0; i < 3; ++i)
		{
			const Term other = Math("MAXIMUM", *axes[(i + 1) % 3], *axes[(i + 2) % 3]) iferr_return;
			const Term difference = Math("SUBTRACT", *axes[i], other) iferr_return;
			const maxon::Int held = _graph.Add("ShaderNodeMath") iferr_return;
			_graph.SetProperty(held, "operation", "MULTIPLY_ADD"_s);
			_graph.SetProperty(held, "use_clamp", true);
			Set(held, "Value", difference, true) iferr_return;
			_graph.SetInput(held, "Value_001", 1.0 / amount);
			_graph.SetInput(held, "Value_002", 0.5);
			weights[i] = Math("POWER", Term::Of(NodeOutput { held, "Value"_s }), Term::Number(0.5 * curve)) iferr_return;
		}
		const Term sum01 = Math("ADD", weights[0], weights[1]) iferr_return;
		const Term sum = Math("ADD", sum01, weights[2]) iferr_return;
		// The texture per axis, its image V upwards (measured on planes facing each way): along x
		// (s z, y), along y (x, s z), along z (-s x, y), s the sign of the normal's component.
		const Term nx = Component(n, 0) iferr_return;
		const Term ny = Component(n, 1) iferr_return;
		const Term nz = Component(n, 2) iferr_return;
		const Term sx = Math("SIGN", nx) iferr_return;
		const Term sy = Math("SIGN", ny) iferr_return;
		const Term sz = Math("SIGN", nz) iferr_return;
		const Term xu = Math("MULTIPLY", sx, pz) iferr_return;
		const Term yv = Math("MULTIPLY", sy, pz) iferr_return;
		const Term zSign = Math("MULTIPLY", sz, Term::Number(-1.0)) iferr_return;
		const Term zu = Math("MULTIPLY", zSign, px) iferr_return;
		Term uvs[3];
		uvs[0] = Combine(xu, py, Term::Number(0.0)) iferr_return;
		uvs[1] = Combine(px, yv, Term::Number(0.0)) iferr_return;
		uvs[2] = Combine(zu, py, Term::Number(0.0)) iferr_return;
		const maxon::Float64 leftHanded = Value(rs, "islefthanded", 1.0) iferr_return;
		const maxon::Float64 yUp = Value(rs, "yisup", 1.0) iferr_return;
		if (leftHanded == 0.0 || yUp == 0.0)
			Warn(FormatString("a TriPlanar node plugged into @ is set for a right-handed or Z-up space, which is not translated; Cinema 4D's axes are used.", maxon::String(what)));
		static const maxon::Char* const IMAGES[3] = { "imagex", "imagey", "imagez" };
		Term result = Term::Of(cinema::Vector(0.0));
		for (maxon::Int i = 0; i < 3; ++i)
		{
			const Term color = Projected(rs, same ? "imagex" : IMAGES[i], colorData, what, depth, uvs[i], " TriPlanar") iferr_return;
			const Term share = Math("DIVIDE", weights[i], sum) iferr_return;
			const Term part = VMath("SCALE", color, share) iferr_return;
			result = VMath("ADD", result, part) iferr_return;
		}
		return Output(result, false);
	}

	handled = false;
	return NodeOutput();
}

} // namespace cyclesrender
