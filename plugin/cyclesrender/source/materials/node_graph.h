// SPDX-License-Identifier: Apache-2.0
// A Blender shader node graph under construction: the plugin's material translators describe
// nodes and links here, in Blender's own names, and the bridge builds them as they are.
//
// Format of the result: protocol/protocol.md, "Node graphs".

#ifndef CYCLESRENDER_NODE_GRAPH_H__
#define CYCLESRENDER_NODE_GRAPH_H__

#include "c4d.h"

#include "../session/json_writer.h"

namespace cyclesrender
{

/// One output socket of a node in the graph. Invalid when default-constructed.
struct NodeOutput
{
	maxon::Int node = NOTOK;
	maxon::String socket;

	maxon::Bool IsValid() const { return node != NOTOK; }
};

//----------------------------------------------------------------------------------------
/// Nodes are referred to by the index Add() returns.
///
///   NodeGraph graph;
///   const maxon::Int bsdf = graph.Add("ShaderNodeBsdfPrincipled") iferr_return;
///   const maxon::Int out = graph.Add("ShaderNodeOutputMaterial") iferr_return;
///   graph.SetInput(bsdf, "Roughness", 0.4);
///   graph.Link({ bsdf, "BSDF"_s }, out, "Surface") iferr_return;
///   graph.Write(json);    // adds the members "nodes" and "links" to the open object
//----------------------------------------------------------------------------------------
class NodeGraph
{
public:
	/// Adds a node of a Blender node type such as "ShaderNodeTexImage".
	maxon::Result<maxon::Int> Add(const maxon::Char* type);

	/// Sets a property of the node itself (not a socket), e.g. "interpolation".
	void SetProperty(maxon::Int node, const maxon::Char* name, const maxon::String& value);
	void SetProperty(maxon::Int node, const maxon::Char* name, maxon::Bool value);
	/// The same with the value given as JSON text, for structured properties (protocol.md,
	/// "Node graphs": a Color Ramp's stops).
	void SetPropertyJson(maxon::Int node, const maxon::Char* name, const maxon::String& json);

	/// Sets the value an input socket has while nothing is linked to it.
	void SetInput(maxon::Int node, const maxon::Char* socket, maxon::Float64 value);
	void SetInput(maxon::Int node, const maxon::Char* socket, const cinema::Vector& value);
	/// The same with the value given as JSON text, for values the bridge fills in (protocol.md,
	/// "Node graphs": numbers that depend on an image).
	void SetInputJson(maxon::Int node, const maxon::Char* socket, const maxon::String& json);

	/// Sets the value an output socket holds, given as JSON text: the Value, RGB and Normal nodes
	/// keep their value there (protocol.md, "Node graphs").
	void SetOutputJson(maxon::Int node, const maxon::Char* socket, const maxon::String& json);

	/// Connects an output to an input socket of @p toNode.
	maxon::Result<void> Link(const NodeOutput& from, maxon::Int toNode, const maxon::Char* toSocket);
	/// The same with the input given by its position, for nodes with several inputs of one name (Mix Shader).
	maxon::Result<void> Link(const NodeOutput& from, maxon::Int toNode, maxon::Int toSocketIndex);

	/// Writes the members "nodes" and "links" into the object that is open in @p json.
	void Write(JsonWriter& json) const;

private:
	struct Node
	{
		maxon::String type;
		JsonWriter properties;
		JsonWriter inputs;
		JsonWriter outputs;
	};

	struct Connection
	{
		NodeOutput from;
		maxon::Int toNode = NOTOK;
		maxon::String toSocket;
		maxon::Int toSocketIndex = NOTOK;   ///< used instead of the name when set
	};

	static maxon::String NodeName(maxon::Int index) { return FormatString("n@", index); }

	maxon::BaseArray<Node> _nodes;
	maxon::BaseArray<Connection> _links;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_NODE_GRAPH_H__
