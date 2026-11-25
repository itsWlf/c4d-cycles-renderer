// SPDX-License-Identifier: Apache-2.0
// Look-alike translations of four Redshift nodes (redshift_nodes.h, LookAlikes): Jitter, Random
// Color Switch, Brick and Flakes. Redshift's own random numbers and noises cannot be reproduced,
// so these render the same kind of picture, not the same pixels: the same ranges, colours,
// proportions and things that vary, measured on Redshift's renders (docs/reference/c4d-sdk.md,
// "Redshift nodes", "Look-alikes"). Each says in the console that it is a look-alike.

#include "c4d.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"

#include "../scene/document_colors.h"
#include "material_export.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

namespace cyclesrender
{

// A 3 x 3 matrix by rows.
struct Rows3
{
	cinema::Vector r[3];
};

static Rows3 Inverse(const Rows3& m)
{
	const cinema::Vector& a = m.r[0];
	const cinema::Vector& b = m.r[1];
	const cinema::Vector& c = m.r[2];
	const cinema::Vector x = Cross(b, c), y = Cross(c, a), z = Cross(a, b);
	const maxon::Float64 det = Dot(a, x);
	Rows3 inverse;
	if (maxon::Abs(det) < 1e-12)
		return m;
	// The inverse's columns are the cross products over the determinant.
	inverse.r[0] = cinema::Vector(x.x, y.x, z.x) / det;
	inverse.r[1] = cinema::Vector(x.y, y.y, z.y) / det;
	inverse.r[2] = cinema::Vector(x.z, y.z, z.z) / det;
	return inverse;
}

// True when a string port holds a text (the "UV Channel" of Brick and Flakes).
static maxon::Bool NamesText(const maxon::GraphNode& port)
{
	if (!port.IsValid())
		return false;
	ifnoerr (const maxon::Opt<const maxon::String&> text = port.GetEffectivePortValue<maxon::String>())
	{
		return text && !text->IsEmpty();
	}
	return false;
}

// Linear Rec.709 from the render space. Redshift's colour jitter works on the HSV of the colour in
// linear Rec.709 (measured in an ACEScg document: the model fits 256 objects within 0.0016, plain
// HSV in ACEScg does not). The matrices are the ACES (AP1, Bradford D60 to D65) and ITU ones.
static maxon::Bool ToRec709(const cinema::BaseDocument* doc, Rows3& m)
{
	if (!doc || !DocumentColors(doc).IsOcio())
		return false;   // Basic colour management calculates in linear sRGB already
	maxon::CString render, display, view, thumbnails;
	doc->GetActiveOcioColorSpacesNames(render, display, view, thumbnails);
	const maxon::String name(render);
	maxon::Int position = 0;
	if (name.Find("ACEScg"_s, &position))
	{
		m.r[0] = cinema::Vector(1.70505099, -0.62179212, -0.08325887);
		m.r[1] = cinema::Vector(-0.13025642, 1.14080474, -0.01054832);
		m.r[2] = cinema::Vector(-0.02400336, -0.12896898, 1.15297233);
		return true;
	}
	if (name.Find("Rec.2020"_s, &position))
	{
		m.r[0] = cinema::Vector(1.66049100, -0.58764114, -0.07284986);
		m.r[1] = cinema::Vector(-0.12455047, 1.13289990, -0.00834942);
		m.r[2] = cinema::Vector(-0.01815076, -0.10057890, 1.11872966);
		return true;
	}
	return false;
}

//----------------------------------------------------------------------------------------
// The random number per object. Measured in Redshift on 256 objects: one number r in 0..1 per
// object and seed drives every output (the float is min + r (max - min), the integer min +
// floor(r (max - min + 1)), the colour jitter's three channels when their seeds are equal, the
// Random Color Switch's choice). In "Name ID" mode r depends on the path of names to the object
// (equal paths, equal r; a render instance and every clone of a Cloner in render-instance or
// multi-instance mode take the value of the object they show; clones in instance mode differ),
// in "Object ID" mode on the Object ID of a Redshift Object tag on the object or above it (no tag:
// all objects one value), in "User Data ID" mode on the node's User Data number, which may be
// wired (an Integer User Data node reading a particle's id gives every particle its own number); the
// Object ID and the User Data number give the same r for the same number. Here r is Blender's White Noise of
// (key, seed, kind), the key sent per object by the plugin (SceneExporter::Leaf::nameKey): other
// numbers than Redshift's, the same structure.
//----------------------------------------------------------------------------------------
maxon::Result<RedshiftNodes::Term> RedshiftNodes::RandomPerObject(const maxon::GraphNode& rsNode, maxon::Float64 seed, const maxon::Char* what,
	const maxon::Char* label)
{
	iferr_scope;
	const maxon::Float64 modeValue = Value(rsNode, "ininputid", 2.0) iferr_return;
	const maxon::Int mode = maxon::Int(modeValue);
	Term key;
	if (mode == 4)
	{
		// The number set on the node, or what is wired into it (a user data node: redshift_scene_data.cpp).
		const Term userData = Input(rsNode, "inuserdata", false, what, 0) iferr_return;
		if (userData.IsConstant())
		{
			key = Term::Of(cinema::Vector(userData.value.x, 0.0, 1.0));
		}
		else
		{
			key = Combine(userData, Term::Number(0.0), Term::Number(1.0)) iferr_return;
		}
	}
	else
	{
		// An Attribute node of type Instancer reads an instance's own number first and falls back
		// to the object's property (protocol: `floats` of an instancer, `properties` of an object).
		const maxon::Int attribute = _graph.Add("ShaderNodeAttribute") iferr_return;
		_graph.SetProperty(attribute, "attribute_type", "INSTANCER"_s);
		_graph.SetProperty(attribute, "attribute_name", maxon::String(mode == 0 ? OBJECT_ID_KEY : OBJECT_NAME_KEY));
		_exporter.UseObjectKeys();
		key = Combine(Term::Of(NodeOutput { attribute, "Fac"_s }), Term::Number(0.0), Term::Number(mode == 0 ? 1.0 : 0.0)) iferr_return;
	}
	const Term shifted = VMath("ADD", key, Term::Of(cinema::Vector(0.0, seed, 0.0))) iferr_return;
	const maxon::Int noise = _graph.Add("ShaderNodeTexWhiteNoise") iferr_return;
	_graph.SetProperty(noise, "noise_dimensions", "3D"_s);
	Set(noise, "Vector", shifted, false) iferr_return;
	return Term::Of(NodeOutput { noise, "Value"_s });
}

maxon::Result<RedshiftNodes::Term> RedshiftNodes::JitterColor(const maxon::GraphNode& rsNode, const Term& color, const maxon::Char* what, const maxon::Char* label)
{
	iferr_scope;
	const maxon::Float64 hueMin = Setting(rsNode, "inhuevarmin", 0.0, what, label) iferr_return;
	const maxon::Float64 hueMax = Setting(rsNode, "inhuevarmax", 1.0, what, label) iferr_return;
	const maxon::Float64 satMin = Setting(rsNode, "insaturationvarmin", 0.0, what, label) iferr_return;
	const maxon::Float64 satMax = Setting(rsNode, "insaturationvarmax", 1.0, what, label) iferr_return;
	const maxon::Float64 valMin = Setting(rsNode, "invaluevarmin", 0.0, what, label) iferr_return;
	const maxon::Float64 valMax = Setting(rsNode, "invaluevarmax", 1.0, what, label) iferr_return;
	if (hueMin == 0.0 && hueMax == 0.0 && satMin == 0.0 && satMax == 0.0 && valMin == 0.0 && valMax == 0.0)
		return color;
	const maxon::Float64 hueSeed = Setting(rsNode, "inhueseed", 123.0, what, label) iferr_return;
	const maxon::Float64 satSeed = Setting(rsNode, "insaturationseed", 123.0, what, label) iferr_return;
	const maxon::Float64 valSeed = Setting(rsNode, "invalueseed", 123.0, what, label) iferr_return;

	// Into linear Rec.709, as three dot products (and back the same way).
	Rows3 toRec709;
	const maxon::Bool convert = ToRec709(_exporter.GetDocument(), toRec709);
	auto transform = [this](const Term& c, const Rows3& m) -> maxon::Result<Term>
	{
		iferr_scope;
		const Term x = VMath("DOT_PRODUCT", c, Term::Of(m.r[0])) iferr_return;
		const Term y = VMath("DOT_PRODUCT", c, Term::Of(m.r[1])) iferr_return;
		const Term z = VMath("DOT_PRODUCT", c, Term::Of(m.r[2])) iferr_return;
		return Combine(x, y, z);
	};
	Term linear = color;
	if (convert)
	{
		linear = transform(color, toRec709) iferr_return;
	}
	const maxon::Int split = _graph.Add("ShaderNodeSeparateColor") iferr_return;
	_graph.SetProperty(split, "mode", "HSV"_s);
	Set(split, "Color", linear, false) iferr_return;

	// Each channel: plus min + r (max - min) of its seed's r; the hue in degrees, wrapped; the
	// saturation and value held to 0..1 (measured).
	auto moved = [&](const maxon::Char* channel, maxon::Float64 low, maxon::Float64 high, maxon::Float64 seed, maxon::Float64 unit, maxon::Bool hue) -> maxon::Result<Term>
	{
		iferr_scope;
		const Term base = Term::Of(NodeOutput { split, maxon::String(channel) });
		if (low == 0.0 && high == 0.0)
			return base;
		const Term r = RandomPerObject(rsNode, seed, what, label) iferr_return;
		const Term amount = Math("MULTIPLY_ADD", r, Term::Number((high - low) * unit), Term::Number(low * unit)) iferr_return;
		const Term sum = Math("ADD", base, amount) iferr_return;
		if (hue)
			return Math("FRACT", sum);
		const Term below = Math("MINIMUM", sum, Term::Number(1.0)) iferr_return;
		return Math("MAXIMUM", below, Term::Number(0.0));
	};
	const Term h = moved("Red", hueMin, hueMax, hueSeed, 1.0 / 360.0, true) iferr_return;
	const Term s = moved("Green", satMin, satMax, satSeed, 1.0, false) iferr_return;
	const Term v = moved("Blue", valMin, valMax, valSeed, 1.0, false) iferr_return;
	const maxon::Int join = _graph.Add("ShaderNodeCombineColor") iferr_return;
	_graph.SetProperty(join, "mode", "HSV"_s);
	Set(join, "Red", h, true) iferr_return;
	Set(join, "Green", s, true) iferr_return;
	Set(join, "Blue", v, true) iferr_return;
	Term result = Term::Of(NodeOutput { join, "Color"_s });
	if (convert)
	{
		result = transform(result, Inverse(toRec709)) iferr_return;
	}
	return result;
}

//----------------------------------------------------------------------------------------
// The four nodes.
//----------------------------------------------------------------------------------------
maxon::Result<NodeOutput> RedshiftNodes::LookAlikes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what,
	maxon::Int depth, maxon::Bool& handled)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	handled = true;

	// Jitter: a float, an integer and a jittered colour per object (RandomPerObject, JitterColor).
	if (name == "jitter"_s)
	{
		Warn(FormatString("the Jitter node plugged into @ is rendered as a look-alike: the same ranges, varying with the same objects, "
			"but other random numbers per object than Redshift's.", maxon::String(what)));
		if (source.output == "outfloat"_s)
		{
			const maxon::Float64 low = Setting(rs, "infloatmin", 0.0, what, " Jitter") iferr_return;
			const maxon::Float64 high = Setting(rs, "infloatmax", 0.0, what, " Jitter") iferr_return;
			const maxon::Float64 seed = Setting(rs, "infloatseed", 123.0, what, " Jitter") iferr_return;
			const Term r = RandomPerObject(rs, seed, what, " Jitter") iferr_return;
			const Term value = Math("MULTIPLY_ADD", r, Term::Number(high - low), Term::Number(low)) iferr_return;
			return Output(value, true);
		}
		if (source.output == "outint"_s)
		{
			// min + floor(r (max - min + 1)), measured: 0 to 3 gives 0, 1, 2 and 3 alike.
			const maxon::Float64 low = Setting(rs, "inintegermin", 0.0, what, " Jitter") iferr_return;
			const maxon::Float64 high = Setting(rs, "inintegermax", 0.0, what, " Jitter") iferr_return;
			const maxon::Float64 seed = Setting(rs, "inintegerseed", 123.0, what, " Jitter") iferr_return;
			const Term r = RandomPerObject(rs, seed, what, " Jitter") iferr_return;
			const Term spread = Math("MULTIPLY", r, Term::Number(maxon::Max(0.0, high - low + 1.0))) iferr_return;
			const Term whole = Math("FLOOR", spread) iferr_return;
			const Term value = Math("ADD", whole, Term::Number(low)) iferr_return;
			return Output(value, true);
		}
		const Term color = Input(rs, "incolor", colorData, what, depth) iferr_return;
		const Term jittered = JitterColor(rs, color, what, " Jitter") iferr_return;
		return Output(jittered, false);
	}

	// Random Color Switch: one of the colours per object, chosen by weight (measured: the colour i
	// whose share of the summed weights holds r: weights 1, 2, 5 gave 14 %, 24 %, 62 % of 256
	// objects, each object exactly where r falls), then the colour jitter if it is on.
	if (name == "randomizedcolorswitch"_s)
	{
		Warn(FormatString("the Random Color Switch node plugged into @ is rendered as a look-alike: the same colours in the same shares, "
			"varying with the same objects, but another choice per object than Redshift's.", maxon::String(what)));
		const maxon::GraphNode shadersPort = Port(rs, "shaders") iferr_return;
		const maxon::BaseArray<maxon::GraphNode> entries = Entries(shadersPort) iferr_return;
		struct Entry { maxon::GraphNode color; maxon::Float64 weight; };
		maxon::BaseArray<Entry> chosen;
		maxon::Float64 total = 0.0;
		for (const maxon::GraphNode& entry : entries)
		{
			const maxon::GraphNode colorPort = entry.FindChild(maxon::Id(RS_NODE "randomizedcolorswitch.struct.shaders.shader_input")) iferr_return;
			const maxon::GraphNode weightPort = entry.FindChild(maxon::Id(RS_NODE "randomizedcolorswitch.struct.shaders.shader_weight")) iferr_return;
			const maxon::Float64 weight = PortNumber(weightPort, 1.0);
			if (weight <= 0.0 || !colorPort.IsValid())
				continue;   // never chosen (measured with a weight of 0)
			chosen.Append(Entry { colorPort, weight }) iferr_return;
			total += weight;
		}
		if (chosen.IsEmpty())
		{
			Warn(FormatString("the Random Color Switch node plugged into @ has no colour with a weight above 0; the value set on the input is used.", maxon::String(what)));
			return NodeOutput();
		}
		const maxon::Float64 seed = Setting(rs, "infloatseed", 123.0, what, " Random Color Switch") iferr_return;
		Term r = Term::Number(0.0);
		if (chosen.GetCount() > 1)
		{
			r = RandomPerObject(rs, seed, what, " Random Color Switch") iferr_return;
		}
		Term result = PortTerm(chosen[0].color, colorData, what, depth) iferr_return;
		maxon::Float64 below = chosen[0].weight;
		for (maxon::Int i = 1; i < chosen.GetCount(); ++i)
		{
			const Term color = PortTerm(chosen[i].color, colorData, what, depth) iferr_return;
			const Term past = Math("GREATER_THAN", r, Term::Number(below / total)) iferr_return;
			result = MixColor("MIX", past, result, color) iferr_return;
			below += chosen[i].weight;
		}
		const maxon::Float64 jitter = Value(rs, "incoljitterenabled", 0.0) iferr_return;
		if (jitter != 0.0)
		{
			result = JitterColor(rs, result, what, " Random Color Switch") iferr_return;
		}
		return Output(result, false);
	}

	if (name == "brick"_s)
		return Brick(source, colorData, what, depth);
	if (name == "flakes"_s)
		return Flakes(source, what);
	if (name == "maxonnoise"_s)
		return MaxonNoise(source, what, depth);

	handled = false;
	return NodeOutput();
}

//----------------------------------------------------------------------------------------
// Brick, measured on Redshift's render of a plane with UVs 0..1 (docs/reference/c4d-sdk.md):
// rows of height h = scale x height and bricks of width w = scale x width in UV units, rows counted
// from v = 0; row r (or r modulo "Reset every nth row") moved by r x shift x w towards -u; every
// "Half Width" row (r modulo n = 0) of bricks half as wide, moved by its own shift x w / 2; the
// alternative colour on rows r modulo "every n rows" = 0, and there only on bricks c with
// (c + 1) modulo "every n columns" = 0 when that is set; the gap w x (gap size + 0.025) wide on
// the border of each brick, thinned and widened by a noise ("Size Variation"), its colour falling
// off like a groove. Each brick's colour is
// its gradient at a smooth noise around 0.5 (spread 0.09, measured) that changes over "Brick Noise
// Scale" x scale, with an offset of its own per brick; the gap's colour likewise at its own scale.
// What Redshift's noises look like inside a brick, its dirt, its textures and the exact gap profile
// are not reproduced.
//----------------------------------------------------------------------------------------
// The gain on Blender's Noise Texture that gives the gradient position Redshift's spread around 0.5
// (measured: 0.09 to 0.096), and how much larger Blender's noise must be drawn to change over the
// same distance as Redshift's (measured by the correlation inside bricks; see the reference).
static const maxon::Float64 BRICK_NOISE_GAIN = 1.1;
static const maxon::Float64 BRICK_NOISE_SIZE = 4.0;
// The gap: Redshift's is brick width x (gap size + 0.025) wide (measured with gap sizes 0.01 to 0.2,
// scales, widths and heights: linear, independent of the height); the mortar colour falls off
// linearly from the middle of the gap to 0.8 of the gap's width from it (a groove, measured).
static const maxon::Float64 BRICK_GAP_OFFSET = 0.025;
static const maxon::Float64 BRICK_GROOVE = 0.8;

maxon::Result<NodeOutput> RedshiftNodes::Brick(const Source& source, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	const maxon::Char* const label = " Brick";
	Warn(FormatString("the Brick node plugged into @ is rendered as a look-alike: Redshift's layout, proportions, gaps and colour gradients, "
		"but noises of Blender's own.", maxon::String(what)));

	// What is not translated, each named once.
	static const maxon::Char* const TEXTURES[] = { "bricktexture", "brickalttexture", "gaptexture", "dirttexture", "dirtcolortexture" };
	for (const maxon::Char* texture : TEXTURES)
	{
		const maxon::GraphNode port = Port(rs, texture) iferr_return;
		const maxon::Bool wired = IsWired(port) iferr_return;
		if (wired)
			Warn(FormatString("the texture plugged into the \"@\" of a Brick node (in @) is not translated.", maxon::String(texture), maxon::String(what)));
	}
	const maxon::Float64 dirt = Value(rs, "dirtenable", 0.0) iferr_return;
	if (dirt != 0.0)
		Warn(FormatString("the dirt of a Brick node plugged into @ is not translated.", maxon::String(what)));
	const maxon::Float64 balance = Value(rs, "balancecolhperiod", 1.0) iferr_return;
	if (balance == 0.0)
		Warn(FormatString("\"Balance Alt. Color Period\" off of a Brick node plugged into @ is not translated.", maxon::String(what)));
	{
		const maxon::GraphNode context = Port(rs, "uv_context") iferr_return;
		const maxon::Bool wired = IsWired(context) iferr_return;
		if (wired)
			Warn(FormatString("the UV context wired into a Brick node plugged into @ is not translated; the object's UV map is used.", maxon::String(what)));
		const maxon::GraphNode channel = Port(rs, "tspace_id") iferr_return;
		if (NamesText(channel))
			Warn(FormatString("the UV channel named by a Brick node plugged into @ is not read; the object's UV map is used.", maxon::String(what)));
	}

	const maxon::Float64 scale = Setting(rs, "brickscaleinput", 0.25, what, label) iferr_return;
	const maxon::Float64 widthSetting = Setting(rs, "brickwidthinput", 0.5, what, label) iferr_return;
	const maxon::Float64 w = maxon::Max(1e-6, scale * widthSetting);
	const maxon::Float64 heightSetting = Setting(rs, "brickheightinput", 0.25, what, label) iferr_return;
	const maxon::Float64 h = maxon::Max(1e-6, scale * heightSetting);
	const maxon::Float64 shift = Setting(rs, "brickshift", 0.5, what, label) iferr_return;
	const maxon::Float64 resetSetting = Setting(rs, "brickshiftreset", 0.0, what, label) iferr_return;
	const maxon::Int reset = maxon::Int(resetSetting);
	const maxon::Float64 halfSetting = Setting(rs, "brickhalfwidthfrequency", 0.0, what, label) iferr_return;
	const maxon::Int halfEvery = maxon::Int(halfSetting);
	const maxon::Float64 halfShift = Setting(rs, "brickhalfwidthshift", 0.5, what, label) iferr_return;
	const maxon::Float64 altRowsSetting = Setting(rs, "altcolorvperiod", 7.0, what, label) iferr_return;
	const maxon::Int altRows = maxon::Int(altRowsSetting);
	const maxon::Float64 altColumnsSetting = Setting(rs, "altcolorhperiod", 0.0, what, label) iferr_return;
	const maxon::Int altColumns = maxon::Int(altColumnsSetting);
	const maxon::Float64 noiseSetting = Setting(rs, "brickcolornoisescaleinput", 0.5, what, label) iferr_return;
	const maxon::Float64 noiseScale = maxon::Max(1e-4, noiseSetting);
	const maxon::Float64 detail = Setting(rs, "detailinput", 0.025, what, label) iferr_return;
	const maxon::Float64 detailScaleSetting = Setting(rs, "detailscaleinput", 0.75, what, label) iferr_return;
	const maxon::Float64 detailScale = maxon::Max(1e-4, detailScaleSetting);
	const maxon::Float64 gapNoiseSetting = Setting(rs, "gapcolornoisescaleinput", 0.005, what, label) iferr_return;
	const maxon::Float64 gapNoiseScale = maxon::Max(1e-5, gapNoiseSetting);
	const maxon::Float64 gapSetting = Setting(rs, "gapsizeinput", 0.025, what, label) iferr_return;
	const maxon::Float64 gap = maxon::Max(0.0, w * (gapSetting + BRICK_GAP_OFFSET));
	const maxon::Float64 variation = Setting(rs, "gapnoiseintensity", 0.9, what, label) iferr_return;
	const maxon::Float64 variationSetting = Setting(rs, "gapnoisescale", 0.25, what, label) iferr_return;
	const maxon::Float64 variationScale = maxon::Max(1e-4, variationSetting);

	// The UV coordinates as Cinema 4D's: Redshift counts the rows from Cinema 4D's v = 0, which is
	// Blender's v = 1 (the plugin sends v upside down, protocol `uvs`; measured: the first alternative
	// row lies where Cinema 4D's v is 0).
	const Term blenderUv = MaterialUv() iferr_return;
	const Term u = Component(blenderUv, 0) iferr_return;
	const Term blenderV = Component(blenderUv, 1) iferr_return;
	const Term v = Math("SUBTRACT", Term::Number(1.0), blenderV) iferr_return;
	const Term uv = Combine(u, v, Term::Number(0.0)) iferr_return;

	// The brick a point is on, and where on it.
	const Term rowAt = Math("DIVIDE", v, Term::Number(h)) iferr_return;
	const Term row = Math("FLOOR", rowAt) iferr_return;
	Term shiftRow = row;
	if (reset > 0)
	{
		shiftRow = Math("FLOORED_MODULO", row, Term::Number(maxon::Float64(reset))) iferr_return;
	}
	Term offset = Math("MULTIPLY", shiftRow, Term::Number(shift * w)) iferr_return;
	Term width = Term::Number(w);
	if (halfEvery > 0)
	{
		const Term place = Math("FLOORED_MODULO", row, Term::Number(maxon::Float64(halfEvery))) iferr_return;
		const Term half = Math("COMPARE", place, Term::Number(0.0), Term::Number(0.5)) iferr_return;
		// width w (1 - half / 2); offset blends to halfShift w / 2 on half rows.
		width = Math("MULTIPLY_ADD", half, Term::Number(-0.5 * w), Term::Number(w)) iferr_return;
		const Term toHalf = Math("SUBTRACT", Term::Number(halfShift * w * 0.5), offset) iferr_return;
		offset = Math("MULTIPLY_ADD", half, toHalf, offset) iferr_return;
	}
	const Term along = Math("ADD", u, offset) iferr_return;
	const Term columnAt = Math("DIVIDE", along, width) iferr_return;
	const Term column = Math("FLOOR", columnAt) iferr_return;
	const Term negativeWidth = Math("MULTIPLY", width, Term::Number(-1.0)) iferr_return;
	const Term s = Math("MULTIPLY_ADD", column, negativeWidth, along) iferr_return;
	const Term t = Math("MULTIPLY_ADD", row, Term::Number(-h), v) iferr_return;
	const Term sFar = Math("SUBTRACT", width, s) iferr_return;
	const Term tFar = Math("SUBTRACT", Term::Number(h), t) iferr_return;
	const Term ds = Math("MINIMUM", s, sFar) iferr_return;
	const Term dt = Math("MINIMUM", t, tFar) iferr_return;
	const Term edge = Math("MINIMUM", ds, dt) iferr_return;

	// A random vector per brick, and noises in pattern units (UV / scale).
	const Term brickCell = Combine(column, row, Term::Number(0.0)) iferr_return;
	const maxon::Int brickRandom = _graph.Add("ShaderNodeTexWhiteNoise") iferr_return;
	_graph.SetProperty(brickRandom, "noise_dimensions", "3D"_s);
	Set(brickRandom, "Vector", brickCell, false) iferr_return;
	const Term perBrick = Term::Of(NodeOutput { brickRandom, "Color"_s });
	auto noise = [&](maxon::Float64 size, const Term& shiftBy, maxon::Float64 detailLevel) -> maxon::Result<Term>
	{
		iferr_scope;
		const Term at = VMath("SCALE", uv, Term::Number(1.0 / maxon::Max(1e-6, scale * size * BRICK_NOISE_SIZE))) iferr_return;
		const Term moved = VMath("ADD", at, shiftBy) iferr_return;
		const maxon::Int node = _graph.Add("ShaderNodeTexNoise") iferr_return;
		_graph.SetProperty(node, "noise_dimensions", "3D"_s);
		_graph.SetInput(node, "Scale", 1.0);
		_graph.SetInput(node, "Detail", detailLevel);
		_graph.SetInput(node, "Roughness", 0.5);
		_graph.SetInput(node, "Distortion", 0.0);
		Set(node, "Vector", moved, false) iferr_return;
		return Term::Of(NodeOutput { node, "Fac"_s });
	};
	auto around = [&](const Term& fac, maxon::Float64 gain) -> maxon::Result<Term>
	{
		// 0.5 + gain (fac - 0.5)
		return Math("MULTIPLY_ADD", fac, Term::Number(gain), Term::Number(0.5 - 0.5 * gain));
	};

	// The gap: its width varies with a noise (measured: "Size Variation" 0.9 leaves 78 % of the
	// mortar of none); the colour's edge is soft, the mask's hard.
	Term gapWidth = Term::Number(gap);
	if (variation != 0.0)
	{
		const Term fac = noise(variationScale, Term::Of(cinema::Vector(11.0, 23.0, 5.0)), 1.0) iferr_return;
		const Term change = Math("MULTIPLY_ADD", fac, Term::Number(2.0 * variation), Term::Number(1.0 - 1.25 * variation)) iferr_return;
		const Term factor = Math("MAXIMUM", change, Term::Number(0.0)) iferr_return;
		gapWidth = Math("MULTIPLY", factor, Term::Number(gap)) iferr_return;
	}
	const Term halfGap = Math("MULTIPLY", gapWidth, Term::Number(0.5)) iferr_return;
	const Term brickMask = Math("GREATER_THAN", edge, halfGap) iferr_return;
	// The mortar colour's share: 1 - edge / (0.8 gap), held to 0..1.
	const Term grooveWidth = Math("MULTIPLY", gapWidth, Term::Number(BRICK_GROOVE)) iferr_return;
	const Term grooveAt = Math("DIVIDE", edge, grooveWidth) iferr_return;
	const Term brickShare = Math("MINIMUM", grooveAt, Term::Number(1.0)) iferr_return;

	if (source.output == "outgapsmask"_s)
		return Output(brickMask, true);
	if (source.output == "outdirtmask"_s)
		return Output(Term::Number(0.0), true);

	if (source.output == "outdisplacementcolor"_s)
	{
		// Each brick at a height of its own with a slope in one of four directions (measured: the
		// middle of a brick at 0.13 to 0.37, the slope across it 0.1 to 0.54), the gap at 0.
		const maxon::Float64 heights = Setting(rs, "brickdisplacementrandomheight", 0.75, what, label) iferr_return;
		const maxon::Float64 slopes = Setting(rs, "brickdisplacementslopeintensity", 0.75, what, label) iferr_return;
		const maxon::Float64 directionSetting = Setting(rs, "brickdisplacementmode", 4.0, what, label) iferr_return;
		const maxon::Int direction = maxon::Int(directionSetting);
		const Term r1 = Component(perBrick, 0) iferr_return;
		const Term r2 = Component(perBrick, 1) iferr_return;
		const Term r3 = Component(perBrick, 2) iferr_return;
		const Term base = Math("MULTIPLY_ADD", r1, Term::Number(-0.22 * heights / 0.75), Term::Number(0.32)) iferr_return;
		const Term steep = Math("MULTIPLY_ADD", r2, Term::Number(0.40 * slopes / 0.75), Term::Number(0.1 * slopes / 0.75)) iferr_return;
		// The position along the slope's direction, -0.5 to 0.5: up 0, down 1, left 2, right 3, random 4.
		const Term across = Math("DIVIDE", s, width) iferr_return;
		const Term upwards = Math("DIVIDE", t, Term::Number(h)) iferr_return;
		auto signedPos = [&](maxon::Int which) -> maxon::Result<Term>
		{
			iferr_scope;
			const Term& p = which < 2 ? upwards : across;
			const Term centred = Math("SUBTRACT", p, Term::Number(0.5)) iferr_return;
			return (which == 1 || which == 2) ? Math("MULTIPLY", centred, Term::Number(-1.0)) : centred;
		};
		Term slope;
		if (direction >= 0 && direction < 4)
		{
			slope = signedPos(direction) iferr_return;
		}
		else
		{
			// One of the four by the brick's third random number.
			const Term quarter = Math("MULTIPLY", r3, Term::Number(3.999)) iferr_return;
			const Term which = Math("FLOOR", quarter) iferr_return;
			slope = signedPos(0) iferr_return;
			for (maxon::Int k = 1; k < 4; ++k)
			{
				const Term other = signedPos(k) iferr_return;
				const Term is = Math("COMPARE", which, Term::Number(maxon::Float64(k)), Term::Number(0.5)) iferr_return;
				const Term difference = Math("SUBTRACT", other, slope) iferr_return;
				slope = Math("MULTIPLY_ADD", is, difference, slope) iferr_return;
			}
		}
		const Term height = Math("MULTIPLY_ADD", steep, slope, base) iferr_return;
		const Term inBrick = Math("MULTIPLY", height, brickMask) iferr_return;
		const Term grey = Combine(inBrick, inBrick, inBrick) iferr_return;
		return Output(grey, false);
	}

	// The colours: each gradient at a noise around 0.5; the brick's noise moved per brick; the
	// detail a finer noise added.
	const Term brickShift = VMath("SCALE", perBrick, Term::Number(37.0)) iferr_return;
	const Term brickFac = noise(noiseScale, brickShift, 2.0) iferr_return;
	Term brickPosition = around(brickFac, BRICK_NOISE_GAIN) iferr_return;
	if (detail != 0.0)
	{
		// A faint grain (measured: "Detail" 0.5 adds a spread of 0.008 at a pixel's scale).
		const Term fine = noise(detailScale * 0.02 / BRICK_NOISE_SIZE, Term::Of(cinema::Vector(3.0, 7.0, 1.0)), 2.0) iferr_return;
		const Term amount = Math("MULTIPLY_ADD", fine, Term::Number(0.2 * detail), Term::Number(-0.1 * detail)) iferr_return;
		brickPosition = Math("ADD", brickPosition, amount) iferr_return;
	}
	const maxon::GraphNode brickGradientPort = Port(rs, "brickcolorinput") iferr_return;
	const NodeOutput brickColor = Gradient(brickGradientPort, brickPosition, what, label) iferr_return;
	Term color = brickColor.IsValid() ? Term::Of(brickColor) : Term::Of(cinema::Vector(0.5));
	if (altRows > 0)
	{
		const maxon::GraphNode altPort = Port(rs, "brickaltcolorinput") iferr_return;
		const NodeOutput altColor = Gradient(altPort, brickPosition, what, label) iferr_return;
		if (altColor.IsValid())
		{
			const Term rowPlace = Math("FLOORED_MODULO", row, Term::Number(maxon::Float64(altRows))) iferr_return;
			Term isAlt = Math("COMPARE", rowPlace, Term::Number(0.0), Term::Number(0.5)) iferr_return;
			if (altColumns > 0)
			{
				const Term next = Math("ADD", column, Term::Number(1.0)) iferr_return;
				const Term columnPlace = Math("FLOORED_MODULO", next, Term::Number(maxon::Float64(altColumns))) iferr_return;
				const Term onColumn = Math("COMPARE", columnPlace, Term::Number(0.0), Term::Number(0.5)) iferr_return;
				isAlt = Math("MULTIPLY", isAlt, onColumn) iferr_return;
			}
			color = MixColor("MIX", isAlt, color, Term::Of(altColor)) iferr_return;
		}
	}
	const Term gapFac = noise(gapNoiseScale, Term::Of(cinema::Vector(5.0, 3.0, 9.0)), 2.0) iferr_return;
	const Term gapPosition = around(gapFac, BRICK_NOISE_GAIN) iferr_return;
	const maxon::GraphNode gapGradientPort = Port(rs, "gapcolorinput") iferr_return;
	const NodeOutput gapColor = Gradient(gapGradientPort, gapPosition, what, label) iferr_return;
	if (gapColor.IsValid())
	{
		color = MixColor("MIX", brickShare, Term::Of(gapColor), color) iferr_return;
	}
	(void)colorData;
	(void)depth;
	return Output(color, false);
}

//----------------------------------------------------------------------------------------
// Flakes, measured on Redshift's renders (docs/reference/c4d-sdk.md): Voronoi cells about "Scale"
// document units apart (object space; world space alike; UV coordinates times 100), of which the
// share "Density" are flakes (alpha 1, measured 0.30 at 0.3, 0.81 at 0.8); "Flakes Size" and "Size
// Variance" change nothing there (measured). Each flake's normal is the surface's tilted by a
// random vector (at "Randomize" 0.5 the tilt's sine has median 0.40, at 1 0.66); a random id per
// flake (0 elsewhere). outNormal is the tilted normal minus the surface's, in world space
// (measured: z = 1 - sqrt(1 - x^2 - y^2) on a plane facing the camera). The "Dots" pattern: round
// dots, 0.38 per Scale squared, of radius 2.2 size^1.63 Scale, overlapping ("Size Variance" changed
// nothing, measured); here four layers of Voronoi dots, so that dots can overlap as Redshift's do.
// Redshift's fade with distance follows the pixel's footprint (measured: it moves with the
// picture's resolution), which a material in Blender cannot read: it is left out.
//----------------------------------------------------------------------------------------
// The length of the random tilt vector for "Randomize" r: 0.56 (2r)^0.9 (fitted to the tilt's quantiles at 0.5 and 1).
static maxon::Float64 FlakeTilt(maxon::Float64 randomize)
{
	return randomize <= 0.0 ? 0.0 : 0.56 * maxon::Pow(2.0 * randomize, 0.9);
}

// Dots: their number per Scale squared, their radius for a size, and the layers drawn.
static const maxon::Float64 DOTS_PER_CELL = 0.38;
static const maxon::Int DOT_LAYERS = 4;
static maxon::Float64 DotRadius(maxon::Float64 size)
{
	return 2.05 * maxon::Pow(maxon::Max(0.0, size), 1.63);
}

maxon::Result<RedshiftNodes::FlakeCells> RedshiftNodes::FlakeCellsOf(const maxon::GraphNode& rs, const maxon::Char* what)
{
	iferr_scope;
	const maxon::Char* const label = " Flakes";
	Warn(FormatString("the Flakes node plugged into @ is rendered as a look-alike: flakes of the same size, share and tilt, "
		"but not Redshift's own, and without its fade with distance.", maxon::String(what)));
	const maxon::Float64 scaleSetting = Setting(rs, "inscale", 1.0, what, label) iferr_return;
	const maxon::Float64 scale = maxon::Max(1e-6, scaleSetting);
	const maxon::Float64 density = Setting(rs, "indensity", 0.8, what, label) iferr_return;
	const maxon::Float64 seed = Setting(rs, "inseed", 123.0, what, label) iferr_return;
	const maxon::Float64 patternValue = Value(rs, "inpattern", 0.0) iferr_return;
	const maxon::Int pattern = maxon::Int(patternValue);
	const maxon::Float64 size = Setting(rs, "inflakesize", 0.5, what, label) iferr_return;
	const maxon::Float64 spaceValue = Value(rs, "incoordinatespace", 1.0) iferr_return;
	const maxon::Int space = maxon::Int(spaceValue);
	const maxon::Float64 depthOf3d = Value(rs, "indepth", 0.0) iferr_return;
	if (depthOf3d > 0.0)
		Warn(FormatString("the 3D flakes (depth, step size, IOR) of a Flakes node plugged into @ are not translated; flat flakes are used.", maxon::String(what)));
	{
		const maxon::GraphNode context = Port(rs, "uv_context") iferr_return;
		const maxon::Bool wired = IsWired(context) iferr_return;
		if (wired)
			Warn(FormatString("the UV context wired into a Flakes node plugged into @ is not translated; the object's UV map is used.", maxon::String(what)));
		const maxon::GraphNode channel = Port(rs, "tspace_id") iferr_return;
		if (space == 2 && NamesText(channel))
			Warn(FormatString("the UV channel named by a Flakes node plugged into @ is not read; the object's UV map is used.", maxon::String(what)));
	}
	if (pattern == 1)
		Warn(FormatString("the dots of a Flakes node plugged into @ end at the borders of Blender's cells where Redshift's would reach further.", maxon::String(what)));

	// The coordinates in cells, moved by the seed.
	Term at;
	if (space == 2)
	{
		const Term uv = MaterialUv() iferr_return;
		at = VMath("SCALE", uv, Term::Number(100.0)) iferr_return;
	}
	else
	{
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		at = ToCinema(Term::Of(NodeOutput { geometry, "Position"_s }), true, space == 0 ? 0 : 1) iferr_return;
	}
	at = VMath("SCALE", at, Term::Number(1.0 / scale)) iferr_return;
	at = VMath("ADD", at, Term::Of(cinema::Vector(0.618, 0.414, 0.732) * (seed * 7.0))) iferr_return;

	// One layer of cells: the Voronoi of the coordinates over @p spacing, moved by @p offset; with a
	// radius, only a dot of that radius around each cell's point.
	struct Layer { Term alpha; Term id; Term random; };
	auto layer = [&](maxon::Float64 spacing, const cinema::Vector& offset, maxon::Float64 radius) -> maxon::Result<Layer>
	{
		iferr_scope;
		Term point = VMath("SCALE", at, Term::Number(1.0 / spacing)) iferr_return;
		point = VMath("ADD", point, Term::Of(offset)) iferr_return;
		const maxon::Int voronoi = _graph.Add("ShaderNodeTexVoronoi") iferr_return;
		_graph.SetProperty(voronoi, "voronoi_dimensions", "3D"_s);
		_graph.SetProperty(voronoi, "feature", "F1"_s);
		_graph.SetInput(voronoi, "Scale", 1.0);
		_graph.SetInput(voronoi, "Randomness", 1.0);
		Set(voronoi, "Vector", point, false) iferr_return;
		const Term cellColor = Term::Of(NodeOutput { voronoi, "Color"_s });
		// An independent random vector per cell, from the cell's point.
		const maxon::Int pointNoise = _graph.Add("ShaderNodeTexWhiteNoise") iferr_return;
		_graph.SetProperty(pointNoise, "noise_dimensions", "3D"_s);
		_graph.Link(NodeOutput { voronoi, "Position"_s }, pointNoise, "Vector") iferr_return;
		Layer result;
		const Term choice = Component(cellColor, 0) iferr_return;
		result.alpha = Math("LESS_THAN", choice, Term::Number(density)) iferr_return;
		if (radius > 0.0)
		{
			const Term distance = Math("MULTIPLY", Term::Of(NodeOutput { voronoi, "Distance"_s }), Term::Number(spacing)) iferr_return;
			const Term inside = Math("LESS_THAN", distance, Term::Number(radius)) iferr_return;
			result.alpha = Math("MULTIPLY", result.alpha, inside) iferr_return;
		}
		const Term idRandom = Component(cellColor, 1) iferr_return;
		result.id = Math("MULTIPLY", idRandom, result.alpha) iferr_return;
		result.random = Term::Of(NodeOutput { pointNoise, "Color"_s });
		return result;
	};

	FlakeCells cells;
	if (pattern == 1)
	{
		// Dots: balls of radius R in space, cut by the surface, which shows discs of radius up to R and
		// on average sqrt(2/3) R; their points are spread so that the surface shows DOTS_PER_CELL discs
		// per Scale squared (2 R of space for each disc), in DOT_LAYERS layers; a later layer's dot
		// shows where no earlier one is.
		const maxon::Float64 radius = DotRadius(size) * maxon::Sqrt(1.5);
		const maxon::Float64 perVolume = DOTS_PER_CELL / maxon::Max(1e-6, 2.0 * radius) / maxon::Float64(DOT_LAYERS);
		const maxon::Float64 spacing = maxon::Pow(1.0 / perVolume, 1.0 / 3.0);
		for (maxon::Int i = 0; i < DOT_LAYERS; ++i)
		{
			const Layer one = layer(spacing, cinema::Vector(0.37, 0.11, 0.73) * maxon::Float64(i * 13), radius) iferr_return;
			if (i == 0)
			{
				cells.alpha = one.alpha;
				cells.id = one.id;
				cells.random = one.random;
				continue;
			}
			const Term empty = Math("SUBTRACT", Term::Number(1.0), cells.alpha) iferr_return;
			const Term take = Math("MULTIPLY", empty, one.alpha) iferr_return;
			cells.id = Math("MULTIPLY_ADD", take, one.id, cells.id) iferr_return;
			cells.random = MixColor("MIX", take, cells.random, one.random) iferr_return;
			cells.alpha = Math("MAXIMUM", cells.alpha, one.alpha) iferr_return;
		}
	}
	else
	{
		const Layer one = layer(1.0, cinema::Vector(0.0), 0.0) iferr_return;
		cells.alpha = one.alpha;
		cells.id = one.id;
		cells.random = one.random;
	}
	cells.blend = Term::Number(1.0);
	return cells;
}

maxon::Result<NodeOutput> RedshiftNodes::Flakes(const Source& source, const maxon::Char* what)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	if (source.output == "outalpha"_s || source.output == "outflakesid"_s || source.output == "outflakesblend"_s)
	{
		const FlakeCells cells = FlakeCellsOf(rs, what) iferr_return;
		if (source.output == "outalpha"_s)
			return Output(cells.alpha, true);
		if (source.output == "outflakesid"_s)
			return Output(cells.id, true);
		return Output(cells.blend, true);
	}
	// outNormal: the tilted normal minus the surface's, in Cinema 4D's world axes.
	const NodeOutput tilted = FlakesNormal(rs, what) iferr_return;
	const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
	const Term difference = VMath("SUBTRACT", Term::Of(tilted), Term::Of(NodeOutput { geometry, "Normal"_s })) iferr_return;
	const Term inCinema = ToCinema(difference, false, 0) iferr_return;
	return Output(inCinema, false);
}

maxon::Result<NodeOutput> RedshiftNodes::FlakesNormal(const maxon::GraphNode& rs, const maxon::Char* what)
{
	iferr_scope;
	const FlakeCells cells = FlakeCellsOf(rs, what) iferr_return;
	const maxon::Float64 randomize = Setting(rs, "inrandomize", 0.5, what, " Flakes") iferr_return;
	const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
	const Term normal = Term::Of(NodeOutput { geometry, "Normal"_s });
	const Term centred = VMath("MULTIPLY_ADD", cells.random, Term::Of(cinema::Vector(2.0)), Term::Of(cinema::Vector(-1.0))) iferr_return;
	const Term strength = Math("MULTIPLY", cells.alpha, Term::Number(FlakeTilt(randomize))) iferr_return;
	const Term push = VMath("SCALE", centred, strength) iferr_return;
	const Term sum = VMath("ADD", normal, push) iferr_return;
	const Term tilted = VMath("NORMALIZE", sum) iferr_return;
	return Output(tilted, false);
}

} // namespace cyclesrender
