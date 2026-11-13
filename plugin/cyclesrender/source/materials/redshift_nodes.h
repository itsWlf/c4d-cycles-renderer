// SPDX-License-Identifier: Apache-2.0
// Redshift's shading nodes wired into a Redshift material, as Blender nodes: the Texture node and
// the nodes with a near one-to-one counterpart in Blender (math, vector math, colour mix, invert,
// constants, colour and vector makers and splitters, colour correction). The list, with what each
// becomes, is in docs/reference/c4d-sdk.md, "Redshift nodes".

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

	/// Names a node that is not translated, for input @p what.
	void NotTranslated(const maxon::GraphNode& node, const maxon::Char* what) const;

	/// A warning about this material.
	void Warn(const maxon::String& text) const;

private:
	struct Source
	{
		maxon::GraphNode node;
		maxon::String output;        ///< the id of its output port, without the node's id
	};

	static maxon::Result<Source> Find(const maxon::GraphNode& port);
	maxon::Result<NodeOutput> Translate(const Source& source, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);
	maxon::Result<NodeOutput> TextureSampler(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what);
	maxon::Result<NodeOutput> ColorCorrection(const maxon::GraphNode& node, maxon::Bool colorData, const maxon::Char* what, maxon::Int depth);

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
