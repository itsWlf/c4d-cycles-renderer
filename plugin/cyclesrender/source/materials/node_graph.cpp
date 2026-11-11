// SPDX-License-Identifier: Apache-2.0
// A Blender node graph under construction, and its JSON form (node_graph.h).

#include "c4d.h"

#include "node_graph.h"

namespace cyclesrender
{

maxon::Result<maxon::Int> NodeGraph::Add(const maxon::Char* type)
{
	iferr_scope;

	Node node;
	node.type = maxon::String(type);
	node.properties.BeginObject();
	node.inputs.BeginObject();
	node.outputs.BeginObject();
	_nodes.Append(std::move(node)) iferr_return;
	return _nodes.GetCount() - 1;
}

void NodeGraph::SetProperty(maxon::Int node, const maxon::Char* name, const maxon::String& value)
{
	_nodes[node].properties.String(name, value);
}

void NodeGraph::SetProperty(maxon::Int node, const maxon::Char* name, maxon::Bool value)
{
	_nodes[node].properties.Bool(name, value);
}

void NodeGraph::SetPropertyJson(maxon::Int node, const maxon::Char* name, const maxon::String& json)
{
	_nodes[node].properties.Raw(name, json);
}

void NodeGraph::SetInput(maxon::Int node, const maxon::Char* socket, maxon::Float64 value)
{
	_nodes[node].inputs.Float(socket, value);
}

void NodeGraph::SetInput(maxon::Int node, const maxon::Char* socket, const cinema::Vector& value)
{
	const maxon::Float64 values[3] = { value.x, value.y, value.z };
	_nodes[node].inputs.Floats(socket, values, 3);
}

void NodeGraph::SetInputJson(maxon::Int node, const maxon::Char* socket, const maxon::String& json)
{
	_nodes[node].inputs.Raw(socket, json);
}

void NodeGraph::SetOutputJson(maxon::Int node, const maxon::Char* socket, const maxon::String& json)
{
	_nodes[node].outputs.Raw(socket, json);
}

maxon::Result<void> NodeGraph::Link(const NodeOutput& from, maxon::Int toNode, const maxon::Char* toSocket)
{
	iferr_scope;

	Connection link;
	link.from = from;
	link.toNode = toNode;
	link.toSocket = maxon::String(toSocket);
	_links.Append(std::move(link)) iferr_return;
	return maxon::OK;
}

maxon::Result<void> NodeGraph::Link(const NodeOutput& from, maxon::Int toNode, maxon::Int toSocketIndex)
{
	iferr_scope;

	Connection link;
	link.from = from;
	link.toNode = toNode;
	link.toSocketIndex = toSocketIndex;
	_links.Append(std::move(link)) iferr_return;
	return maxon::OK;
}

void NodeGraph::Write(JsonWriter& json) const
{
	json.BeginArray("nodes");
	for (maxon::Int i = 0; i < _nodes.GetCount(); ++i)
	{
		const Node& node = _nodes[i];
		json.BeginObject();
		json.String("name", NodeName(i));
		json.String("type", node.type);
		// The three writers hold an object that is still open.
		json.Raw("props", node.properties.GetText() + "}"_s);
		json.Raw("inputs", node.inputs.GetText() + "}"_s);
		json.Raw("outputs", node.outputs.GetText() + "}"_s);
		json.EndObject();
	}
	json.EndArray();

	json.BeginArray("links");
	for (const Connection& link : _links)
	{
		json.BeginArray();
		json.String(nullptr, NodeName(link.from.node));
		json.String(nullptr, link.from.socket);
		json.String(nullptr, NodeName(link.toNode));
		if (link.toSocketIndex != NOTOK)
			json.Int(nullptr, link.toSocketIndex);
		else
			json.String(nullptr, link.toSocket);
		json.EndArray();
	}
	json.EndArray();
}

} // namespace cyclesrender
