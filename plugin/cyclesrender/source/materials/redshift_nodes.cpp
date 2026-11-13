// SPDX-License-Identifier: Apache-2.0
// Redshift's shading nodes as Blender nodes (redshift_nodes.h). Reading order: finding what is
// wired into a port, the tables of nodes that become one Blender node, then Translate, which
// handles every node it knows, and the Texture node.

#include "c4d.h"
#include "c4d_basematerial.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/url.h"

#include "material_export.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

namespace cyclesrender
{

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
	const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
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
			Warn(FormatString("the alpha output of a Color Splitter plugged into @ is not translated (Blender's colours have none); the value set on the input is used.", maxon::String(what)));
			return NodeOutput();
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
// the saturation at 1 where Redshift does not. The settings count with their values; what is wired
// into them is named and left out.
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
	const maxon::Float64 gamma = setting("gamma", 1.0) iferr_return;
	const maxon::Float64 contrast = setting("contrast", 0.5) iferr_return;
	const maxon::Float64 hue = setting("hue", 0.0) iferr_return;
	const maxon::Float64 saturation = setting("saturation", 1.0) iferr_return;
	const maxon::Float64 level = setting("level", 1.0) iferr_return;

	// The input, through a Gamma node that may change nothing (it holds the input).
	const maxon::Int gammaNode = _graph.Add("ShaderNodeGamma") iferr_return;
	_graph.SetInput(gammaNode, "Gamma", gamma > 0.0 ? 1.0 / gamma : 1.0);
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
	if (hue != 0.0 || saturation != 1.0 || level != 1.0)
	{
		if (saturation > 1.0)
			Warn(FormatString("a Color Correct node plugged into @ raises the saturation above 1, which Blender clamps at 1.", maxon::String(what)));
		const maxon::Int hsv = _graph.Add("ShaderNodeHueSaturation") iferr_return;
		_graph.SetInput(hsv, "Hue", 0.5 + hue / 360.0 - maxon::Floor(0.5 + hue / 360.0));
		_graph.SetInput(hsv, "Saturation", saturation);
		_graph.SetInput(hsv, "Value", level);
		_graph.SetInput(hsv, "Fac", 1.0);
		_graph.Link(color, hsv, "Color") iferr_return;
		color = NodeOutput { hsv, "Color"_s };
	}
	return color;
}

maxon::Result<NodeOutput> RedshiftNodes::TextureSampler(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what)
{
	iferr_scope;
	const maxon::GraphNode inputs = node.GetInputs() iferr_return;
	const maxon::GraphNode bundle = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.tex0")) iferr_return;
	maxon::GraphNode pathPort;
	if (bundle.IsValid())
	{
		pathPort = bundle.FindChild(maxon::Id("path")) iferr_return;
	}
	maxon::String path;
	if (pathPort.IsValid())
	{
		ifnoerr (const maxon::Opt<const maxon::Url&> url = pathPort.GetEffectivePortValue<maxon::Url>())
		{
			if (url && url->IsPopulated())
			{
				path = url->GetSystemPath() iferr_return;
			}
		}
		if (path.IsEmpty())
		{
			ifnoerr (const maxon::Opt<const maxon::String&> text = pathPort.GetEffectivePortValue<maxon::String>())
			{
				if (text)
					path = *text;
			}
		}
	}
	if (path.IsEmpty())
	{
		Warn(FormatString("the Texture node plugged into @ names no file.", maxon::String(what)));
		return NodeOutput();
	}
	const maxon::String image = _exporter.GetFileImageId(path, colorData) iferr_return;
	const maxon::Int texture = _graph.Add("ShaderNodeTexImage") iferr_return;
	_graph.SetProperty(texture, "image", image);

	// The texture's own tiling and offset.
	const maxon::GraphNode scalePort = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.scale")) iferr_return;
	const maxon::GraphNode offsetPort = inputs.FindChild(maxon::Id(RS_NODE "texturesampler.offset")) iferr_return;
	maxon::Vector2d scale(1.0);
	maxon::Vector2d offset(0.0);
	if (scalePort.IsValid())
	{
		ifnoerr (const maxon::Opt<const maxon::Vector2d&> value = scalePort.GetEffectivePortValue<maxon::Vector2d>())
		{
			if (value)
				scale = *value;
		}
	}
	if (offsetPort.IsValid())
	{
		ifnoerr (const maxon::Opt<const maxon::Vector2d&> moved = offsetPort.GetEffectivePortValue<maxon::Vector2d>())
		{
			if (moved)
				offset = *moved;
		}
	}
	if (scale != maxon::Vector2d(1.0) || offset != maxon::Vector2d(0.0))
	{
		const maxon::Int coordinates = _graph.Add("ShaderNodeTexCoord") iferr_return;
		const maxon::Int mapping = _graph.Add("ShaderNodeMapping") iferr_return;
		_graph.SetInput(mapping, "Scale", cinema::Vector(scale.x, scale.y, 1.0));
		_graph.SetInput(mapping, "Location", cinema::Vector(offset.x, offset.y, 0.0));
		_graph.Link(NodeOutput { coordinates, "UV"_s }, mapping, "Vector") iferr_return;
		_graph.Link(NodeOutput { mapping, "Vector"_s }, texture, "Vector") iferr_return;
	}
	return NodeOutput { texture, "Color"_s };
}

} // namespace cyclesrender
