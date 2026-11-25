// SPDX-License-Identifier: Apache-2.0
// Redshift's shading nodes wired into a Redshift material, as Blender nodes: the Texture node, the
// math, colour, utility and texture nodes with a counterpart in Blender (among them the
// inverse trigonometry, bias and gain, the vector and colour math, Color Composite and Color Layer,
// Fresnel, State, Ramp and Scalar Ramp, TriPlanar, UV Projection, MatCap, Camera Map and others),
// each measured against Redshift's own render, and the nodes that read data of the scene by name
// (user data, vertex and point attributes). The list, with what each becomes, is in
// docs/reference/c4d-sdk.md, "Redshift nodes".

#ifndef CYCLESRENDER_REDSHIFT_NODES_H__
#define CYCLESRENDER_REDSHIFT_NODES_H__

#include "c4d.h"
#include "maxon/graph.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

//----------------------------------------------------------------------------------------
/// Translates what is wired into a port of a Redshift node, node by node, into @p graph.
/// A node it cannot translate is named in the console (through the exporter) and stands for
/// nothing: the caller then uses the value set on its input.
//----------------------------------------------------------------------------------------
class RedshiftNodes
{
public:
	RedshiftNodes(MaterialExporter& exporter, NodeGraph& graph, const cinema::BaseMaterial* material)
		: _exporter(exporter), _graph(graph), _material(material) { }

	/// The Blender output for what is wired into @p port; invalid when nothing is, or when what
	/// is wired cannot be translated (then a warning names it). @p colorData: an image read
	/// there holds colours (sRGB), not data. @p what names the input in warnings.
	maxon::Result<NodeOutput> Wired(const maxon::GraphNode& port, maxon::Bool colorData, const maxon::Char* what);

	/// The asset id of a node, without Redshift's prefix ("texturesampler"); and the node with the
	/// output port wired into @p port. For the reader of the Standard Material (redshift_material.cpp).
	static maxon::Result<maxon::String> ShortName(const maxon::GraphNode& node);
	static maxon::Result<maxon::GraphNode> SourceNode(const maxon::GraphNode& port);

	/// The file named by the "tex0" bundle of a node of @p asset (its short id), as the id of an
	/// `image` item; empty, with a warning naming @p label, when it names none.
	maxon::Result<maxon::String> FileImage(const maxon::GraphNode& node, const maxon::String& asset, maxon::Bool colorData, const maxon::Char* what, const maxon::Char* label);

	/// Names a node that is not translated, for input @p what.
	void NotTranslated(const maxon::GraphNode& node, const maxon::Char* what) const;

	/// The shading normal (Blender's world space) a Flakes node gives on a material's bump input:
	/// the surface's normal tilted per flake (redshift_lookalikes.cpp). Invalid when it cannot be made.
	maxon::Result<NodeOutput> FlakesNormal(const maxon::GraphNode& node, const maxon::Char* what);

	/// A warning about this material.
	void Warn(const maxon::String& text) const;

	/// A value inside a translation: the output of a Blender node, or a constant (a number is its
	/// x). @p alpha is the alpha of a constant colour; a wired colour counts as opaque.
	struct Term
	{
		NodeOutput out;
		cinema::Vector value;
		maxon::Float64 alpha = 1.0;

		maxon::Bool IsConstant() const { return !out.IsValid(); }
		static Term Number(maxon::Float64 number) { Term term; term.value = cinema::Vector(number); return term; }
		static Term Of(const cinema::Vector& vector) { Term term; term.value = vector; return term; }
		static Term Of(const NodeOutput& output) { Term term; term.out = output; return term; }
	};

private:
	/// A node whose output is wired into a port, and which of its outputs it is.
	struct Source
	{
		maxon::GraphNode node;
		maxon::String output;        ///< the id of its output port, without the node's id
	};

	/// What is wired into @p port; an invalid node when nothing is.
	static maxon::Result<Source> Find(const maxon::GraphNode& port);
	/// The Blender output for @p source, or invalid (named in a warning) when it cannot be translated.
	/// @p depth counts the nodes on the way from the material, against graphs that loop.
	maxon::Result<NodeOutput> Translate(const Source& source, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);
	/// A Texture node as an Image Texture, on its UV map, on the texture tag's projection or on @p vector (Blender UV
	/// coordinates a projection node computes), with the node's own rotation, offset and scale and then the tag's offset and
	/// length (measured, 0.3.0 F13 W18), its colour space (the node's, not @p colorData's, where it names one), colour
	/// multiplier, offset and gamma.
	maxon::Result<NodeOutput> TextureSampler(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what, const Term* vector = nullptr);
	/// The UV coordinates of the material as the texture tag it sits on places them (MaterialExporter::GetCurrentMapping):
	/// its projection, offset and length, repeated into 0..1 when the tag tiles; the UV map itself for plain UVW mapping and
	/// for a cubic projection. For the nodes
	/// that read UV coordinates besides the Texture node (Ramp, Brick, Flakes, Maxon Noise).
	maxon::Result<Term> MaterialUv();
	/// What is wired into @p port of a projection node, sampled at @p vector when it is a Texture
	/// node; anything else is translated as it is and named as not projected.
	maxon::Result<Term> Projected(const maxon::GraphNode& rsNode, const maxon::Char* port, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth,
		const Term& vector, const maxon::Char* label);
	/// The position along a Ramp or Scalar Ramp (0 to 1): its source, mapping and adjustments.
	maxon::Result<Term> RampPosition(const maxon::GraphNode& rsNode, const maxon::String& asset, const maxon::Char* what, maxon::Int depth);
	/// Color Correct: gamma, contrast, hue and saturation, level (redshift_nodes.cpp has the formulas).
	maxon::Result<NodeOutput> ColorCorrection(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);

	// The groups of nodes Translate hands on; each answers false in @p handled for a node it does not know.
	maxon::Result<NodeOutput> MoreMath(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth, maxon::Bool& handled);
	maxon::Result<NodeOutput> ColorNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth, maxon::Bool& handled);
	maxon::Result<NodeOutput> UtilityNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth, maxon::Bool& handled);
	maxon::Result<NodeOutput> TextureNodes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth, maxon::Bool& handled);
	/// The look-alikes (redshift_lookalikes.cpp): Jitter, Random Color Switch, Brick, Flakes. Each
	/// renders the same kind of picture as Redshift without Redshift's own random numbers, and says so.
	maxon::Result<NodeOutput> LookAlikes(const Source& source, const maxon::String& name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth, maxon::Bool& handled);
	/// The nodes that read data of the scene by a name (redshift_scene_data.cpp): the Color, Scalar,
	/// Integer and Vector User Data nodes (what is known per particle and per clone, an object's
	/// Object ID and display colour, and its own user data), the Vertex Attribute and the Point
	/// Attribute node (a mesh's vertex maps and vertex colours).
	maxon::Result<NodeOutput> SceneDataNodes(const Source& source, const maxon::String& name, const maxon::Char* what, maxon::Bool& handled);
	/// What a user data node reads: as a colour or vector (@p size 3) or a number (1), as one number
	/// (a colour's mean), and its alpha. @p found is false when its default is to be used.
	struct SceneValue
	{
		Term value;
		Term number;
		Term alpha;
		Term has;                    ///< 1 where the shading point has the data, 0 where the node's default is to be used (PresenceAttribute)
		maxon::Int size = 0;
		maxon::Bool found = false;
	};
	/// @p read where @p has is 1, @p fallback where it is 0 (a number when @p scalar, else a colour or vector).
	maxon::Result<Term> Present(const Term& has, const Term& read, const Term& fallback, maxon::Bool scalar);
	maxon::Result<SceneValue> ReadUserData(const maxon::GraphNode& rsNode, const maxon::Char* what);
	/// The output @p socket ("Color", "Vector", "Fac") of an Attribute node for the attribute @p name:
	/// with @p both read from the geometry and from the instancer and added up (a missing attribute
	/// reads as 0, so one material is right on a point cloud and on instances), else from the
	/// instancer alone (which falls back to the object's property).
	maxon::Result<Term> AttributeTerm(const maxon::String& name, const maxon::Char* socket, maxon::Bool both);
	/// The alpha of the colour @p source gives, for a Color Splitter's alpha output: of a Color User
	/// Data node the alpha of what it reads, of a Vertex or Point Attribute node the vertex colour's.
	/// @p known turns false for any other node: the alpha of its colour is not followed.
	maxon::Result<Term> AlphaOf(const Source& source, const maxon::Char* what, maxon::Bool& known);
	/// A random number in 0..1 per object for @p seed, as the random nodes' "Input ID Mode" says:
	/// the object's name key, its Object ID, or the node's User Data number, which may be wired
	/// (a user data node reading a particle's id: one number per particle); the same number and
	/// seed give the same value, as in Redshift.
	maxon::Result<Term> RandomPerObject(const maxon::GraphNode& rsNode, maxon::Float64 seed, const maxon::Char* what, const maxon::Char* label);
	/// Redshift's colour jitter (measured): hue, saturation and value of the colour in linear
	/// Rec.709 moved by the node's ranges at the random numbers of their seeds.
	maxon::Result<Term> JitterColor(const maxon::GraphNode& rsNode, const Term& color, const maxon::Char* what, const maxon::Char* label);
	maxon::Result<NodeOutput> Brick(const Source& source, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);
	/// The Maxon Noise node as a look-alike (redshift_noise.cpp, 0.3.0 F13 W14): per noise type the scenes use, Blender's Noise
	/// or Voronoi Texture with the type's measured mean, spread and feature size; invalid (named) for another type.
	maxon::Result<NodeOutput> MaxonNoise(const Source& source, const maxon::Char* what, maxon::Int depth);
	maxon::Result<NodeOutput> Flakes(const Source& source, const maxon::Char* what);
	/// The flakes: per Voronoi cell of the node's pattern, whether it is a flake (x), a random
	/// number (y) and the cell's random vector (the colour of Blender's Voronoi), and the fade with distance.
	struct FlakeCells { Term alpha; Term id; Term random; Term blend; };
	maxon::Result<FlakeCells> FlakeCellsOf(const maxon::GraphNode& rsNode, const maxon::Char* what);
	/// A gradient bundle port (the Ramp's, the Brick's colours) as a Color Ramp at @p t.
	maxon::Result<NodeOutput> Gradient(const maxon::GraphNode& port, const Term& t, const maxon::Char* what, const maxon::Char* label);
	/// The children of a variadic bundle port, in their order.
	static maxon::Result<maxon::BaseArray<maxon::GraphNode>> Entries(const maxon::GraphNode& port);
	/// What a port holds (as Input), the number set on it, the input port @p name of @p rsNode, and whether something is wired into a port.
	maxon::Result<Term> PortTerm(const maxon::GraphNode& port, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);
	static maxon::Float64 PortNumber(const maxon::GraphNode& port, maxon::Float64 fallback);
	static maxon::Result<maxon::GraphNode> Port(const maxon::GraphNode& rsNode, const maxon::Char* name);
	static maxon::Result<maxon::Bool> IsWired(const maxon::GraphNode& port);

	// --- Terms: building expressions of Blender nodes (constant parts are folded where it is plain)
	/// What the input @p name of @p rsNode holds: the translation of what is wired into it (alpha 1),
	/// else its value with its alpha.
	maxon::Result<Term> Input(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);
	/// The value set on an input, ignoring what is wired; names a wire as "only its value is translated".
	maxon::Result<maxon::Float64> Setting(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Float64 fallback, const maxon::Char* what, const maxon::Char* label);
	/// The value of an input that is never wired in practice (a switch, a mode), without a warning.
	maxon::Result<maxon::Float64> Value(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Float64 fallback);
	maxon::Result<cinema::Vector> VectorValue(const maxon::GraphNode& rsNode, const maxon::Char* name, const cinema::Vector& fallback);
	maxon::Result<void> Set(maxon::Int node, const maxon::Char* socket, const Term& term, maxon::Bool scalar);
	/// Blender's Math node; @p b and @p c as far as the operation uses them.
	maxon::Result<Term> Math(const maxon::Char* operation, const Term& a, const Term& b = Term(), const Term& c = Term());
	/// Blender's Vector Math node; SCALE takes its factor in @p b (a number).
	maxon::Result<Term> VMath(const maxon::Char* operation, const Term& a, const Term& b = Term(), const Term& c = Term());
	/// Blender's Mix node on colours, factor not clamped.
	maxon::Result<Term> MixColor(const maxon::Char* blend, const Term& factor, const Term& a, const Term& b);
	/// One component (0, 1, 2) of a vector or colour, and three numbers as a vector.
	maxon::Result<Term> Component(const Term& vector, maxon::Int index);
	maxon::Result<Term> Combine(const Term& x, const Term& y, const Term& z);
	/// The same function of one number applied to each component.
	maxon::Result<Term> PerComponent(const Term& a, const Term& b, maxon::Result<Term> (RedshiftNodes::*function)(const Term&, const Term&));
	/// Perlin's bias x^(log b / log 0.5) and gain, as Redshift computes them (measured).
	maxon::Result<Term> Bias(const Term& x, const Term& b);
	maxon::Result<Term> Gain(const Term& x, const Term& g);
	maxon::Result<Term> Logarithm(const Term& x, const Term& base);
	/// A Blender output for a term: a constant becomes a Value or Combine XYZ node.
	maxon::Result<NodeOutput> Output(const Term& term, maxon::Bool scalar);
	/// A blend mode of Redshift's Color Composite (0 Base Color ... 16 Divide) on two colours.
	maxon::Result<Term> Blend(maxon::Int mode, const Term& base, const Term& blend);
	/// A vector of Blender's world space in Cinema 4D's axes, in document units for a @p point,
	/// in world (0), object (1) or camera (2) space.
	maxon::Result<Term> ToCinema(const Term& vector, maxon::Bool point, maxon::Int space);
	/// The surface's tangent along its UV map (Blender's world space).
	maxon::Result<Term> UvTangent();

	/// Gives the Blender socket @p socket of @p node what the Redshift input @p name of @p rsNode
	/// has: the translation of what is wired into it, else its value.
	maxon::Result<void> Feed(const maxon::GraphNode& rsNode, const maxon::Char* name, maxon::Int node, const maxon::Char* socket,
		maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);

	MaterialExporter& _exporter;
	NodeGraph& _graph;
	const cinema::BaseMaterial* _material;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_NODES_H__
