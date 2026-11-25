// SPDX-License-Identifier: Apache-2.0
// The Maxon Noise node as a look-alike (redshift_nodes.h, MaxonNoise; 0.3.0, F13 W14). Redshift's Maxon Noise is Cinema 4D's
// family of noises, whose algorithms Blender does not have: each type the parity scenes use becomes Blender's Noise Texture
// (fBM) or Voronoi Texture with the type's mean, spread and feature size as Redshift renders them (measured on planes,
// internal/f13/probe_w14.py and analyse_w14.py; docs/reference/c4d-sdk.md, "Look-alikes"), the same kind of picture, not
// the same pixels. The node's own settings that change the picture as a whole (the overall and per-axis scale, the
// source, the clips, brightness, contrast, the two colours) are translated as measured; what is not is named.

#include "c4d.h"
#include "maxon/graph_basics.h"
#include "maxon/nodesgraph.h"

#include "../scene/coords.h"
#include "material_export.h"
#include "redshift_nodes.h"

#define RS_NODE "com.redshift3d.redshift4c4d.nodes.core."

namespace cyclesrender
{

// A noise type as Blender's texture: Voronoi or fBM noise, whether it takes the node's octaves (else one octave whatever the
// node says, as Redshift's type "Noise" does: measured, 1, 5 and 8 octaves alike), its distortion, how much finer its
// features are than Blender's texture's at the frequency of the type "Noise", Redshift's mean and spread of the values
// (at the default output settings), and the spread of Blender's texture as set up here (so that the values are stretched
// to Redshift's spread).
struct NoiseLook
{
	maxon::Int type;
	const maxon::Char* name;
	maxon::Bool voronoi;
	maxon::Bool octaves;
	maxon::Float64 distortion;
	maxon::Float64 finer;
	maxon::Float64 mean;
	maxon::Float64 spread;
	maxon::Float64 blenderSpread;
};

// Measured with Redshift 2027.0.0 and fitted with Blender 5.2.3 at Overall Scale 5 (and 10) on the UV map
// (internal/f13/probe_w14.py, sets "types" and "sizes"; analyse_w14.py).
static const NoiseLook NOISE_LOOKS[] =
{
	{ 13, "Noise", false, false, 0.0, 1.65, 0.506, 0.145, 0.139 },
	{ 8, "FBM", false, true, 0.0, 1.59, 0.499, 0.108, 0.0844 },
	{ 21, "Turbulence", false, true, 0.0, 1.41, 0.502, 0.093, 0.0843 },
	{ 23, "Wavy turbulence", false, true, 0.6, 0.92, 0.494, 0.128, 0.080 },
	{ 22, "VL noise", false, true, 0.8, 0.82, 0.507, 0.145, 0.080 },
	{ 12, "Naki", false, true, 0.3, 1.41, 0.467, 0.141, 0.0828 },
	{ 10, "Luka", false, true, 0.0, 3.38, 0.398, 0.122, 0.0813 },
	{ 5, "Cranal", false, true, 0.4, 2.22, 0.584, 0.080, 0.080 },
	{ 3, "Buya", false, true, 0.0, 2.53, 0.112, 0.139, 0.0679 },
	{ 17, "Poxo", false, true, 0.0, 3.38, 0.172, 0.080, 0.081 },
	{ 27, "Voronoi 1", true, false, 0.0, 1.32, 0.492, 0.169, 0.162 },
	{ 25, "Displaced voronoi", true, false, 0.3, 1.26, 0.493, 0.168, 0.1625 },
};

// The frequency of Blender's texture per unit of the UV map and per centimetre of the object at Overall Scale 1 (the type
// "Noise" fitted to Redshift's feature size), and the mean of Blender's Voronoi F1 distance.
static const maxon::Float64 NOISE_FREQUENCY_UV = 30.0;
static const maxon::Float64 NOISE_FREQUENCY_CM = 4.9;
static const maxon::Float64 BLENDER_VORONOI_MEAN = 0.512;

maxon::Result<NodeOutput> RedshiftNodes::MaxonNoise(const Source& source, const maxon::Char* what, maxon::Int depth)
{
	iferr_scope;
	const maxon::GraphNode& rs = source.node;
	const maxon::Float64 typeValue = Value(rs, "noise_type", 13.0) iferr_return;
	const maxon::Int type = maxon::Int(typeValue);
	const NoiseLook* look = nullptr;
	for (const NoiseLook& entry : NOISE_LOOKS)
	{
		if (entry.type == type)
			look = &entry;
	}
	if (!look)
	{
		Warn(FormatString("the Maxon Noise node plugged into @ has a noise type (@) that is not translated; the value set on the input is used.", maxon::String(what), type));
		return NodeOutput();
	}
	Warn(FormatString("the Maxon Noise node (@) plugged into @ is rendered as a look-alike: Blender's @ with the type's mean, spread and feature size, "
		"not Redshift's pattern.", maxon::String(look->name), maxon::String(what), look->voronoi ? "Voronoi Texture"_s : "Noise Texture"_s));

	// Where: the UV map (as the texture tag places it), the object or the world (centimetres, Cinema 4D's axes); divided by
	// the overall scale times the per-axis scale (measured: a larger scale, larger features), moved by the offset.
	const maxon::Float64 sourceKind = Value(rs, "coord_source", 2.0) iferr_return;
	const maxon::Float64 overallSetting = Setting(rs, "coord_scale_global", 100.0, what, " Maxon Noise") iferr_return;
	const maxon::Float64 overall = maxon::Max(1e-6, overallSetting);
	const cinema::Vector axes = VectorValue(rs, "coord_scale", cinema::Vector(1.0)) iferr_return;
	const cinema::Vector offset = VectorValue(rs, "coord_offset", cinema::Vector(0.0)) iferr_return;
	const cinema::Vector rotation = VectorValue(rs, "coord_rotation", cinema::Vector(0.0)) iferr_return;
	Term position;
	maxon::Float64 frequency = 0.0;
	if (sourceKind == 0.0)
	{
		position = MaterialUv() iferr_return;
		frequency = NOISE_FREQUENCY_UV;
	}
	else
	{
		const maxon::Int geometry = _graph.Add("ShaderNodeNewGeometry") iferr_return;
		position = ToCinema(Term::Of(NodeOutput { geometry, "Position"_s }), true, sourceKind == 3.0 ? 0 : 1) iferr_return;
		// Per document unit: the frequency per centimetre times the centimetres of a unit.
		frequency = NOISE_FREQUENCY_CM * Coords::FromDocument(_exporter.GetDocument()).GetScale() * 100.0;
	}
	if (rotation != cinema::Vector(0.0))
		Warn(FormatString("the rotation of a Maxon Noise node plugged into @ is not translated.", maxon::String(what)));
	const cinema::Vector scale(frequency * look->finer / (overall * maxon::Max(1e-6, axes.x)), frequency * look->finer / (overall * maxon::Max(1e-6, axes.y)),
		frequency * look->finer / (overall * maxon::Max(1e-6, axes.z)));
	// The seed moves the pattern (another seed, another pattern, as in Redshift; not Redshift's).
	const maxon::Float64 seed = Value(rs, "seed", 123.0) iferr_return;
	const cinema::Vector seeded(maxon::FMod(seed * 7.13, 97.0), maxon::FMod(seed * 3.71, 89.0), maxon::FMod(seed * 5.37, 83.0));
	const Term at = VMath("MULTIPLY_ADD", position, Term::Of(scale), Term::Of(cinema::Vector(offset.x * scale.x, offset.y * scale.y, offset.z * scale.z) + seeded)) iferr_return;

	// The pattern: Blender's texture, its values moved to the type's mean and spread.
	Term value;
	if (look->voronoi)
	{
		const maxon::Int voronoi = _graph.Add("ShaderNodeTexVoronoi") iferr_return;
		_graph.SetProperty(voronoi, "voronoi_dimensions", "3D"_s);
		_graph.SetProperty(voronoi, "feature", "F1"_s);
		_graph.SetInput(voronoi, "Scale", 1.0);
		_graph.SetInput(voronoi, "Randomness", 1.0);
		Set(voronoi, "Vector", at, false) iferr_return;
		value = Math("MULTIPLY_ADD", Term::Of(NodeOutput { voronoi, "Distance"_s }), Term::Number(look->spread / look->blenderSpread),
			Term::Number(look->mean - BLENDER_VORONOI_MEAN * look->spread / look->blenderSpread)) iferr_return;
	}
	else
	{
		const maxon::Float64 octavesSetting = Setting(rs, "octaves", 5.0, what, " Maxon Noise") iferr_return;
		const maxon::Float64 octaves = look->octaves ? maxon::Max(1.0, octavesSetting) : 1.0;
		const maxon::Float64 lacunarity = Setting(rs, "lacunarity", 2.1, what, " Maxon Noise") iferr_return;
		const maxon::Float64 gain = Setting(rs, "gain", 0.5, what, " Maxon Noise") iferr_return;
		const maxon::Int noise = _graph.Add("ShaderNodeTexNoise") iferr_return;
		_graph.SetProperty(noise, "noise_dimensions", "3D"_s);
		_graph.SetInput(noise, "Scale", 1.0);
		_graph.SetInput(noise, "Detail", maxon::Min(15.0, octaves - 1.0));
		_graph.SetInput(noise, "Roughness", gain);
		_graph.SetInput(noise, "Lacunarity", lacunarity);
		_graph.SetInput(noise, "Distortion", look->distortion);
		Set(noise, "Vector", at, false) iferr_return;
		value = Math("MULTIPLY_ADD", Term::Of(NodeOutput { noise, "Fac"_s }), Term::Number(look->spread / look->blenderSpread),
			Term::Number(look->mean - 0.5 * look->spread / look->blenderSpread)) iferr_return;
	}

	// The output settings (measured on "Noise"): the clips stretch low..high to 0..1, the brightness adds, the contrast
	// spreads around 0.5 by 1 / (1 - contrast) (a negative contrast narrows by 1 + contrast [assumed]); each held to 0..1.
	const maxon::Float64 low = Setting(rs, "low_clip", 0.0, what, " Maxon Noise") iferr_return;
	const maxon::Float64 high = Setting(rs, "high_clip", 1.0, what, " Maxon Noise") iferr_return;
	const maxon::Float64 brightness = Setting(rs, "brightness", 0.0, what, " Maxon Noise") iferr_return;
	const maxon::Float64 contrast = Setting(rs, "contrast", 0.0, what, " Maxon Noise") iferr_return;
	if (low != 0.0 || high != 1.0)
	{
		const maxon::Float64 width = maxon::Max(1e-6, high - low);
		value = Math("MULTIPLY_ADD", value, Term::Number(1.0 / width), Term::Number(-low / width)) iferr_return;
	}
	value = Math("ADD", value, Term::Number(brightness)) iferr_return;
	const maxon::Float64 stretch = contrast >= 0.0 ? 1.0 / maxon::Max(1e-6, 1.0 - contrast) : 1.0 + contrast;
	if (stretch != 1.0)
	{
		value = Math("MULTIPLY_ADD", value, Term::Number(stretch), Term::Number(0.5 - 0.5 * stretch)) iferr_return;
	}
	const Term floor = Math("MAXIMUM", value, Term::Number(0.0)) iferr_return;
	value = Math("MINIMUM", floor, Term::Number(1.0)) iferr_return;
	const maxon::Float64 cycles = Setting(rs, "cycles", 0.0, what, " Maxon Noise") iferr_return;
	const maxon::Float64 absolute = Value(rs, "absolute", 0.0) iferr_return;
	const maxon::Float64 speed = Value(rs, "animation_speed", 0.0) iferr_return;
	if (cycles != 0.0 || absolute != 0.0 || speed != 0.0)
		Warn(FormatString("the cycles, the absolute values and the animation of a Maxon Noise node plugged into @ are not translated.", maxon::String(what)));

	// The two colours, mixed by the value.
	const Term first = Input(rs, "color1", true, what, depth) iferr_return;
	const Term second = Input(rs, "color2", true, what, depth) iferr_return;
	const Term color = MixColor("MIX", value, first, second) iferr_return;
	return Output(color, false);
}

} // namespace cyclesrender
