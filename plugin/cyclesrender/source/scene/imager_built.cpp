// SPDX-License-Identifier: Apache-2.0
// The Imager's effects the plugin builds of several of Blender's compositor nodes (imager_built.h).

#include "c4d.h"

#include "../materials/node_graph.h"
#include "imager_built.h"

namespace cyclesrender
{

// The value of the setting @p identifier of @p effect, or nullptr when the table has no such setting.
static const ImagerChain::Value* Find(const ImagerEffect& effect, const ImagerChain::Value* values, const maxon::Char* identifier,
	const ImagerSetting** setting = nullptr)
{
	for (maxon::Int32 i = 0; i < effect.settingCount; ++i)
	{
		if (maxon::String(effect.settings[i].identifier) == maxon::String(identifier))
		{
			if (setting)
				*setting = &effect.settings[i];
			return &values[i];
		}
	}
	return nullptr;
}

static maxon::Float64 Number(const ImagerEffect& effect, const ImagerChain::Value* values, const maxon::Char* identifier, maxon::Float64 fallback)
{
	const ImagerChain::Value* const value = Find(effect, values, identifier);
	return value ? value->number[0] : fallback;
}

static cinema::Vector Colour(const ImagerEffect& effect, const ImagerChain::Value* values, const maxon::Char* identifier, const cinema::Vector& fallback)
{
	const ImagerChain::Value* const value = Find(effect, values, identifier);
	return value ? cinema::Vector(value->number[0], value->number[1], value->number[2]) : fallback;
}

// Blender's name of the option chosen for a menu setting, or @p fallback.
static maxon::String Option(const ImagerEffect& effect, const ImagerChain::Value* values, const maxon::Char* identifier, const maxon::String& fallback)
{
	const ImagerSetting* setting = nullptr;
	const ImagerChain::Value* const value = Find(effect, values, identifier, &setting);
	return value ? GetImagerOption(*setting, value->option) : fallback;
}

// A Math node of @p operation fed by @p a (and @p b, or the number @p second).
static maxon::Result<NodeOutput> Math(NodeGraph& graph, const maxon::Char* operation, const NodeOutput& a, maxon::Float64 second,
	const NodeOutput& b = NodeOutput())
{
	iferr_scope;
	const maxon::Int node = graph.Add("ShaderNodeMath") iferr_return;
	graph.SetProperty(node, "operation", maxon::String(operation));
	graph.Link(a, node, "Value") iferr_return;
	if (b.IsValid())
	{
		graph.Link(b, node, "Value_001") iferr_return;
	}
	else
	{
		graph.SetInput(node, "Value_001", second);
	}
	return NodeOutput{ node, "Value"_s };
}

// The Filter node of @p type on @p input: per channel the size of the change across a pixel.
static maxon::Result<NodeOutput> Filter(NodeGraph& graph, const maxon::String& type, const NodeOutput& input)
{
	iferr_scope;
	const maxon::Int node = graph.Add("CompositorNodeFilter") iferr_return;
	graph.SetInputJson(node, "Type", JsonQuoted(type));
	graph.SetInput(node, "Fac", 1.0);
	graph.Link(input, node, "Image") iferr_return;
	return NodeOutput{ node, "Image"_s };
}

//----------------------------------------------------------------------------------------
// "Edges": lines where the depth, the normals or the picture change, in a colour over the picture.
//
// Each source becomes a number that is 1 at a change of a known size: an edge filter (Sobel and its kin
// give 4 s across a step of s) of the depth's logarithm (a step of 10 % in distance, whatever the
// distance: so a far edge counts as a near one), of the normals (a crease of 30 degrees: the normals
// differ by 2 sin 15 degrees), or of the logarithm of the picture's brightness (a step of 25 %). The
// largest of them, through a ramp from 0.75 to 1.25 times the threshold (the lines' edges anti-aliased),
// widened by Width pixels, mixed in by Strength. Measured: docs/reference/blender.md, "Edges".
//----------------------------------------------------------------------------------------
static const maxon::Float64 EDGE_DEPTH_UNIT = 4.0 * 0.0953;		// 4 ln(1.1)
static const maxon::Float64 EDGE_NORMAL_UNIT = 4.0 * 0.5176;	// 4 * 2 sin(15 degrees)
static const maxon::Float64 EDGE_PICTURE_UNIT = 4.0 * 0.2231;	// 4 ln(1.25)
static const maxon::Float64 EDGE_DARKEST = 0.01;							// added to the brightness before its logarithm (black has none)
static const maxon::Float64 EULER = 2.718281828459045;

static maxon::Result<NodeOutput> BuildEdges(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const maxon::String& layerName, const NodeOutput& picture)
{
	iferr_scope;
	NodeGraph& graph = context.graph;
	maxon::String source = Option(effect, values, "Source", "Depth and Normal"_s);
	const maxon::String filter = Option(effect, values, "Filter", "Sobel"_s);
	if (source != "Picture"_s && context.target.viewport && !context.target.eevee)
	{
		// Measured (docs/reference/blender.md, "Edges"): Blender's viewport hands its compositor no passes of Cycles,
		// only the picture. So the IPR with Cycles draws the picture's edges; final frames and Eevee use the passes.
		context.writer.Warn(FormatString("Imager: the layer \"@\" draws the edges of the picture in the IPR with Cycles, not of the @ (Blender's "
			"viewport gives Cycles' compositor no passes); final frames and Eevee use them.", layerName, source == "Depth"_s ? "depth"_s :
			source == "Normal"_s ? "normals"_s : "depth and normals"_s));
		source = "Picture"_s;
	}
	const maxon::Bool depth = source == "Depth"_s || source == "Depth and Normal"_s;
	const maxon::Bool normal = source == "Normal"_s || source == "Depth and Normal"_s;

	NodeOutput edge;
	if (depth)
	{
		if (!context.passes.Contains("use_pass_z"_s))
		{
			context.passes.Append("use_pass_z"_s) iferr_return;
		}
		const NodeOutput logarithm = Math(graph, "LOGARITHM", { context.render, "Depth"_s }, EULER) iferr_return;
		const NodeOutput filtered = Filter(graph, filter, logarithm) iferr_return;
		edge = Math(graph, "MULTIPLY", filtered, 1.0 / EDGE_DEPTH_UNIT) iferr_return;
	}
	if (normal)
	{
		if (!context.passes.Contains("use_pass_normal"_s))
		{
			context.passes.Append("use_pass_normal"_s) iferr_return;
		}
		const NodeOutput filtered = Filter(graph, filter, { context.render, "Normal"_s }) iferr_return;
		const maxon::Int length = graph.Add("ShaderNodeVectorMath") iferr_return;
		graph.SetProperty(length, "operation", "LENGTH"_s);
		graph.Link(filtered, length, "Vector") iferr_return;
		const NodeOutput scaled = Math(graph, "MULTIPLY", { length, "Value"_s }, 1.0 / EDGE_NORMAL_UNIT) iferr_return;
		if (edge.IsValid())
		{
			edge = Math(graph, "MAXIMUM", edge, 0.0, scaled) iferr_return;
		}
		else
		{
			edge = scaled;
		}
	}
	if (!depth && !normal)
	{
		const maxon::Int bw = graph.Add("CompositorNodeRGBToBW") iferr_return;
		graph.Link(picture, bw, "Image") iferr_return;
		const NodeOutput lifted = Math(graph, "ADD", { bw, "Val"_s }, EDGE_DARKEST) iferr_return;
		const NodeOutput logarithm = Math(graph, "LOGARITHM", lifted, EULER) iferr_return;
		const NodeOutput filtered = Filter(graph, filter, logarithm) iferr_return;
		edge = Math(graph, "MULTIPLY", filtered, 1.0 / EDGE_PICTURE_UNIT) iferr_return;
	}

	const maxon::Float64 threshold = maxon::Max(Number(effect, values, "Threshold", 1.0), 1e-4);
	const maxon::Int ramp = graph.Add("ShaderNodeMapRange") iferr_return;
	graph.Link(edge, ramp, "Value") iferr_return;
	graph.SetInput(ramp, "From Min", threshold * 0.75);
	graph.SetInput(ramp, "From Max", threshold * 1.25);
	graph.SetInput(ramp, "To Min", 0.0);
	graph.SetInput(ramp, "To Max", 1.0);
	graph.SetProperty(ramp, "clamp", true);

	const maxon::Int widen = graph.Add("CompositorNodeDilateErode") iferr_return;
	graph.SetInputJson(widen, "Type", "\"Steps\""_s);
	graph.SetInputJson(widen, "Size", maxon::String::IntToString(maxon::Int64(maxon::Max(Number(effect, values, "Width", 0.0), 0.0) + 0.5)));
	graph.Link({ ramp, "Result"_s }, widen, "Mask") iferr_return;
	const NodeOutput factor = Math(graph, "MULTIPLY", { widen, "Mask"_s }, maxon::Clamp01(Number(effect, values, "Strength", 1.0))) iferr_return;

	const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
	graph.SetProperty(mix, "data_type", "RGBA"_s);
	graph.SetProperty(mix, "blend_type", "MIX"_s);
	graph.Link(factor, mix, "Factor_Float") iferr_return;
	graph.Link(picture, mix, "A_Color") iferr_return;
	graph.SetInput(mix, "B_Color", context.colors.Input(Colour(effect, values, "Color", cinema::Vector(0.0))));
	return NodeOutput{ mix, "Result_Color"_s };
}

// A Mix node of colours with @p blend: @p a, and @p b (or the colour @p colour), by @p factor.
static maxon::Result<maxon::Int> MixColours(NodeGraph& graph, const maxon::Char* blend, const NodeOutput& a, const NodeOutput& b, const cinema::Vector& colour,
	maxon::Float64 factor)
{
	iferr_scope;
	const maxon::Int mix = graph.Add("ShaderNodeMix") iferr_return;
	graph.SetProperty(mix, "data_type", "RGBA"_s);
	graph.SetProperty(mix, "blend_type", maxon::String(blend));
	graph.SetInput(mix, "Factor_Float", factor);
	graph.Link(a, mix, "A_Color") iferr_return;
	if (b.IsValid())
	{
		graph.Link(b, mix, "B_Color") iferr_return;
	}
	else
	{
		graph.SetInput(mix, "B_Color", colour);
	}
	return mix;
}

// Redshift's luminance (measured: the Rec.709 weights on the render-space values, docs/reference/c4d-sdk.md) of the
// channels of @p separated (a Separate Color node in RGB).
static const maxon::Float64 LUMINANCE[3] = { 0.2126, 0.7152, 0.0722 };

static maxon::Result<NodeOutput> Luminance(NodeGraph& graph, maxon::Int separated)
{
	iferr_scope;
	const maxon::Char* const channels[3] = { "Red", "Green", "Blue" };
	NodeOutput sum = Math(graph, "MULTIPLY", { separated, "Red"_s }, LUMINANCE[0]) iferr_return;
	for (maxon::Int i = 1; i < 3; ++i)
	{
		const maxon::Int node = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(node, "operation", "MULTIPLY_ADD"_s);
		graph.Link({ separated, maxon::String(channels[i]) }, node, "Value") iferr_return;
		graph.SetInput(node, "Value_001", LUMINANCE[i]);
		graph.Link(sum, node, "Value_002") iferr_return;
		sum = { node, "Value"_s };
	}
	return sum;
}

//----------------------------------------------------------------------------------------
// "Tone Mapping": Redshift's camera tone mapping, measured exact in Redshift 2027.0 (an OCIO document, 17 grey values
// from 0.01 to 16 at four Highlights, coloured with and without Desaturate Highlights): y = H x + (1 - H) x / (1 + x),
// per channel with Desaturate Highlights, else the colour times y(L) / L of its luminance L. Then the saturation s:
// every channel L' + (c - L') (1 + s), at least 0, L' the luminance after the compression.
//----------------------------------------------------------------------------------------
static maxon::Result<NodeOutput> BuildToneMapping(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const NodeOutput& picture)
{
	iferr_scope;
	NodeGraph& graph = context.graph;
	const maxon::Float64 highlights = maxon::Clamp01(Number(effect, values, "Highlights", 0.2));
	const maxon::Bool perChannel = Number(effect, values, "Desaturate Highlights", 1.0) != 0.0;
	const maxon::Float64 saturation = maxon::ClampValue(Number(effect, values, "Saturation", 0.0), -1.0, 1.0);

	NodeOutput compressed;
	if (perChannel)
	{
		const maxon::Int plusOne = MixColours(graph, "ADD", picture, NodeOutput(), cinema::Vector(1.0), 1.0) iferr_return;
		const maxon::Int ratio = MixColours(graph, "DIVIDE", picture, { plusOne, "Result_Color"_s }, cinema::Vector(), 1.0) iferr_return;
		const maxon::Int kept = MixColours(graph, "MIX", { ratio, "Result_Color"_s }, picture, cinema::Vector(), highlights) iferr_return;
		compressed = { kept, "Result_Color"_s };
	}
	else
	{
		// The factor y(L) / L = H + (1 - H) / (1 + L), on the whole colour.
		const maxon::Int separate = graph.Add("CompositorNodeSeparateColor") iferr_return;
		graph.Link(picture, separate, "Image") iferr_return;
		const NodeOutput luminance = Luminance(graph, separate) iferr_return;
		const NodeOutput onePlus = Math(graph, "ADD", luminance, 1.0) iferr_return;
		const maxon::Int share = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(share, "operation", "DIVIDE"_s);
		graph.SetInput(share, "Value", 1.0 - highlights);
		graph.Link(onePlus, share, "Value_001") iferr_return;
		const NodeOutput factor = Math(graph, "ADD", { share, "Value"_s }, highlights) iferr_return;
		const maxon::Int scaled = MixColours(graph, "MULTIPLY", picture, factor, cinema::Vector(), 1.0) iferr_return;
		compressed = { scaled, "Result_Color"_s };
	}

	// The saturation, around the luminance of the compressed colour (there at 0 too: another value is never another tree).
	const maxon::Float64 gain = 1.0 + saturation;
	const maxon::Int separate = graph.Add("CompositorNodeSeparateColor") iferr_return;
	graph.Link(compressed, separate, "Image") iferr_return;
	const NodeOutput luminance = Luminance(graph, separate) iferr_return;
	const NodeOutput grey = Math(graph, "MULTIPLY", luminance, 1.0 - gain) iferr_return;
	const maxon::Int combine = graph.Add("CompositorNodeCombineColor") iferr_return;
	const maxon::Char* const channels[3] = { "Red", "Green", "Blue" };
	for (maxon::Int i = 0; i < 3; ++i)
	{
		const maxon::Int channel = graph.Add("ShaderNodeMath") iferr_return;
		graph.SetProperty(channel, "operation", "MULTIPLY_ADD"_s);
		graph.Link({ separate, maxon::String(channels[i]) }, channel, "Value") iferr_return;
		graph.SetInput(channel, "Value_001", gain);
		graph.Link(grey, channel, "Value_002") iferr_return;
		const NodeOutput positive = Math(graph, "MAXIMUM", { channel, "Value"_s }, 0.0) iferr_return;
		graph.Link(positive, combine, channels[i]) iferr_return;
	}
	graph.Link({ separate, "Alpha"_s }, combine, "Alpha") iferr_return;
	return NodeOutput{ combine, "Image"_s };
}

//----------------------------------------------------------------------------------------
// "White Balance": Redshift's camera White Point, measured exact: every channel divided by the white point's, the
// result scaled so that grey keeps its brightness (the Rec.709 luminance of 1 / white point becomes 1).
//----------------------------------------------------------------------------------------
static maxon::Result<NodeOutput> BuildWhiteBalance(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const NodeOutput& picture)
{
	iferr_scope;
	const cinema::Vector white = context.colors.Input(Colour(effect, values, "White Point", cinema::Vector(1.0)));
	cinema::Vector inverse(1.0 / maxon::Max(white.x, 1e-4), 1.0 / maxon::Max(white.y, 1e-4), 1.0 / maxon::Max(white.z, 1e-4));
	const maxon::Float64 luminance = LUMINANCE[0] * inverse.x + LUMINANCE[1] * inverse.y + LUMINANCE[2] * inverse.z;
	inverse /= luminance;
	const maxon::Int mix = MixColours(context.graph, "MULTIPLY", picture, NodeOutput(), inverse, 1.0) iferr_return;
	return NodeOutput{ mix, "Result_Color"_s };
}

//----------------------------------------------------------------------------------------
// "Lens Vignetting": every pixel times cos(theta)^Strength, theta the angle of its ray to the camera's axis, which is
// Redshift's camera Vignetting measured exact (four focal lengths, square, landscape and portrait pictures, to 0.001:
// docs/reference/c4d-sdk.md). tan(theta) is the pixel's distance from the centre over half the longer side (Blender's
// Uniform coordinates, pixel centres) times tan(Field of View / 2); cos^n = (1 + tan^2)^(-n / 2).
//----------------------------------------------------------------------------------------
static maxon::Result<NodeOutput> BuildLensVignetting(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const NodeOutput& picture)
{
	iferr_scope;
	NodeGraph& graph = context.graph;
	const maxon::Float64 strength = maxon::Max(Number(effect, values, "Strength", 4.0), 0.0);
	const maxon::Float64 view = maxon::ClampValue(Number(effect, values, "Field of View", 0.9273), 0.0175, 3.124);
	const maxon::Int coordinates = graph.Add("CompositorNodeImageCoordinates") iferr_return;
	graph.Link(picture, coordinates, "Image") iferr_return;
	const maxon::Int length = graph.Add("ShaderNodeVectorMath") iferr_return;
	graph.SetProperty(length, "operation", "LENGTH"_s);
	graph.Link({ coordinates, "Uniform"_s }, length, "Vector") iferr_return;
	const NodeOutput tangent = Math(graph, "MULTIPLY", { length, "Value"_s }, maxon::Tan(view * 0.5)) iferr_return;
	const NodeOutput squared = Math(graph, "MULTIPLY", tangent, 0.0, tangent) iferr_return;
	const NodeOutput onePlus = Math(graph, "ADD", squared, 1.0) iferr_return;
	const NodeOutput factor = Math(graph, "POWER", onePlus, -0.5 * strength) iferr_return;
	const maxon::Int mix = MixColours(graph, "MULTIPLY", picture, factor, cinema::Vector(), 1.0) iferr_return;
	return NodeOutput{ mix, "Result_Color"_s };
}

//----------------------------------------------------------------------------------------
// "Lens Glare": Blender's Glare node at its full Strength, and of what it adds Intensity times (a Mix whose factor
// may pass 1: the picture plus Intensity times the glare's difference to it).
//----------------------------------------------------------------------------------------
static maxon::Result<NodeOutput> BuildLensGlare(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const NodeOutput& picture)
{
	iferr_scope;
	NodeGraph& graph = context.graph;
	const maxon::Int glare = graph.Add("CompositorNodeGlare") iferr_return;
	graph.SetInputJson(glare, "Type", JsonQuoted(Option(effect, values, "Type", "Bloom"_s)));
	graph.SetInputJson(glare, "Quality", "\"High\""_s);
	graph.SetInput(glare, "Highlights Threshold", maxon::Max(Number(effect, values, "Threshold", 1.0), 0.0));
	graph.SetInput(glare, "Highlights Smoothness", maxon::Clamp01(Number(effect, values, "Smoothness", 0.1)));
	graph.SetInput(glare, "Strength", 1.0);
	graph.SetInput(glare, "Size", maxon::Clamp01(Number(effect, values, "Size", 0.5)));
	graph.SetInput(glare, "Saturation", maxon::Clamp01(Number(effect, values, "Saturation", 1.0)));
	graph.SetInput(glare, "Tint", context.colors.Input(Colour(effect, values, "Tint", cinema::Vector(1.0))));
	graph.SetInputJson(glare, "Streaks", maxon::String::IntToString(maxon::Int64(Number(effect, values, "Streaks", 4.0))));
	graph.SetInput(glare, "Streaks Angle", Number(effect, values, "Streaks Angle", 0.0));
	graph.SetInputJson(glare, "Iterations", maxon::String::IntToString(maxon::Int64(Number(effect, values, "Iterations", 3.0))));
	graph.SetInput(glare, "Fade", maxon::ClampValue(Number(effect, values, "Fade", 0.9), 0.75, 1.0));
	graph.Link(picture, glare, "Image") iferr_return;
	const maxon::Int mix = MixColours(graph, "MIX", picture, { glare, "Image"_s }, cinema::Vector(), maxon::Max(Number(effect, values, "Intensity", 1.0), 0.0)) iferr_return;
	graph.SetProperty(mix, "clamp_factor", false);
	return NodeOutput{ mix, "Result_Color"_s };
}

maxon::Result<NodeOutput> BuildImagerEffect(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const maxon::String& layerName, const NodeOutput& picture)
{
	iferr_scope;
	const maxon::String name(effect.name);
	if (name == "Edges"_s)
		return BuildEdges(context, effect, values, layerName, picture);
	if (name == "Tone Mapping"_s)
		return BuildToneMapping(context, effect, values, picture);
	if (name == "White Balance"_s)
		return BuildWhiteBalance(context, effect, values, picture);
	if (name == "Lens Glare"_s)
		return BuildLensGlare(context, effect, values, picture);
	if (name == "Lens Vignetting"_s)
		return BuildLensVignetting(context, effect, values, picture);
	// A built effect this file does not know is a fault of the table's generator.
	context.writer.Warn(FormatString("Imager: the effect \"@\" (layer \"@\") is not built by this version of the plugin; it is left out.", name, layerName));
	return picture;
}

} // namespace cyclesrender
