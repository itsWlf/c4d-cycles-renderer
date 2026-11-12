// SPDX-License-Identifier: Apache-2.0
// Writes a material of Cycles nodes as Blender's node graph, node for node
// (cycles_node_material.h).

#include "c4d.h"
#include "c4d_basematerial.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/graph.h"
#include "maxon/graph_basics.h"
#include "maxon/hashmap.h"
#include "maxon/nodesgraph.h"
#include "maxon/url.h"

#include "../nodespace/cycles_nodes.h"
#include "cycles_curves.h"
#include "cycles_node_material.h"
#include "material_export.h"

namespace cyclesrender
{

// A node of the Cinema 4D graph and where it went.
struct PlacedNode
{
	maxon::GraphNode node;
	const CyclesNode* definition = nullptr;
	maxon::Int index = NOTOK;     ///< in the Blender graph; NOTOK for the Material Output
};

static maxon::Result<const CyclesNode*> DefinitionOf(const maxon::GraphNode& node)
{
	iferr_scope;
	const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
	return FindCyclesNode(asset.Get<0>());
}

static maxon::Result<maxon::Id> PortId(const maxon::Char* text)
{
	iferr_scope;
	maxon::Id id;
	id.Init(maxon::String(text)) iferr_return;
	return id;
}

// The output port wired into an input port; invalid when nothing is.
static maxon::Result<maxon::GraphNode> WiredOutput(const maxon::GraphNode& port)
{
	iferr_scope;
	maxon::GraphNode output;
	port.GetConnections(maxon::PORT_DIR::INPUT, [&output](const maxon::GraphConnection& connection) -> maxon::Result<maxon::Bool>
		{
			output = connection.first;
			return false;
		}) iferr_return;
	return output;
}

// The value of a setting on a node, as JSON text.
static maxon::Result<maxon::String> PropertyJson(const maxon::GraphNode& port, const CyclesProperty& property)
{
	iferr_scope;
	switch (property.kind)
	{
		case 'e':
		{
			maxon::Id fallback;
			fallback.Init(maxon::String(property.text)) iferr_return;
			const maxon::Id value = port.GetEffectivePortValue<maxon::Id>().GetValueOr(fallback) iferr_return;
			return "\""_s + value.ToString() + "\""_s;
		}
		case 'b':
		{
			const maxon::Bool value = port.GetEffectivePortValue<maxon::Bool>().GetValueOr(property.number != 0.0) iferr_return;
			return value ? "true"_s : "false"_s;
		}
		case 'i':
		{
			const maxon::Int value = port.GetEffectivePortValue<maxon::Int>().GetValueOr(maxon::Int(property.number)) iferr_return;
			return maxon::String::IntToString(value);
		}
		case 'f':
		{
			const maxon::Float value = port.GetEffectivePortValue<maxon::Float>().GetValueOr(maxon::Float(property.number)) iferr_return;
			return maxon::String::FloatToString(value, -1, 6);
		}
		case 'c':
		{
			const maxon::Vector fallback = GetCyclesPropertyVector(property);
			const maxon::Color value = port.GetEffectivePortValue<maxon::Color>().GetValueOr(maxon::Color(fallback.x, fallback.y, fallback.z)) iferr_return;
			return FormatString("[@, @, @]", maxon::String::FloatToString(value.r, -1, 6), maxon::String::FloatToString(value.g, -1, 6),
				maxon::String::FloatToString(value.b, -1, 6));
		}
		case 'v':
		{
			const maxon::Vector value = port.GetEffectivePortValue<maxon::Vector>().GetValueOr(GetCyclesPropertyVector(property)) iferr_return;
			return FormatString("[@, @, @]", maxon::String::FloatToString(value.x, -1, 6), maxon::String::FloatToString(value.y, -1, 6),
				maxon::String::FloatToString(value.z, -1, 6));
		}
		default:
		{
			const maxon::String value = port.GetEffectivePortValue<maxon::String>().GetValueOr(maxon::String(property.text)) iferr_return;
			return "\""_s + value + "\""_s;
		}
	}
}

// The stops of a Color Ramp node as the node's `color_ramp` (protocol.md, "Node graphs").
// Cinema 4D's node descriptions have no gradient control, so the stops are plain settings of
// the node: "ramp:stops" says how many count, and each has "ramp:position_<n>",
// "ramp:color_<n>" and "ramp:alpha_<n>" (n from 1, one digit). Blender wants them in rising
// order of position.
static maxon::Result<maxon::String> ColorRampJson(const maxon::GraphNode& inputs, const CyclesNode& definition)
{
	iferr_scope;

	struct Stop
	{
		maxon::Float position = 0.0;
		maxon::Color color;
		maxon::Float alpha = 1.0;
	};
	static const maxon::Int MAX_STOPS = 9;
	Stop stops[MAX_STOPS];
	maxon::Int count = 2;
	maxon::String interpolation = "LINEAR"_s;

	for (maxon::Int i = 0; i < definition.propertyCount; ++i)
	{
		const CyclesProperty& property = definition.properties[i];
		const maxon::String name(property.identifier);
		if (!name.StartsWith("ramp:"_s))
			continue;
		const maxon::Id id = PortId(property.port) iferr_return;
		const maxon::GraphNode port = inputs.FindChild(id) iferr_return;
		if (!port.IsValid())
			continue;
		if (property.kind == 'e')
		{
			maxon::Id fallback;
			fallback.Init(maxon::String(property.text)) iferr_return;
			const maxon::Id chosenId = port.GetEffectivePortValue<maxon::Id>().GetValueOr(fallback) iferr_return;
			const maxon::String chosen = chosenId.ToString();
			if (name == "ramp:interpolation"_s)
				interpolation = chosen;
			else if (chosen.GetLength() == 2)   // "ramp:stops": "s2" to "s8"
				count = maxon::ClampValue(maxon::Int(chosen[1]) - maxon::Int('0'), maxon::Int(1), MAX_STOPS);
			continue;
		}
		const maxon::Int index = maxon::Int(name[name.GetLength() - 1]) - maxon::Int('1');
		if (index < 0 || index >= MAX_STOPS)
			continue;
		if (property.kind == 'c')
		{
			const maxon::Vector fallback = GetCyclesPropertyVector(property);
			stops[index].color = port.GetEffectivePortValue<maxon::Color>().GetValueOr(maxon::Color(fallback.x, fallback.y, fallback.z)) iferr_return;
		}
		else
		{
			const maxon::Float value = port.GetEffectivePortValue<maxon::Float>().GetValueOr(maxon::Float(property.number)) iferr_return;
			if (name.StartsWith("ramp:position_"_s))
				stops[index].position = maxon::ClampValue(value, 0.0, 1.0);
			else
				stops[index].alpha = value;
		}
	}

	// In rising order of position; stops at one position keep their order.
	for (maxon::Int i = 1; i < count; ++i)
	{
		for (maxon::Int k = i; k > 0 && stops[k].position < stops[k - 1].position; --k)
			std::swap(stops[k], stops[k - 1]);
	}

	maxon::String json = "{\"interpolation\": \""_s + interpolation + "\", \"elements\": ["_s;
	for (maxon::Int i = 0; i < count; ++i)
	{
		const Stop& stop = stops[i];
		json += FormatString("@[@, @, @, @, @]", i > 0 ? ", "_s : ""_s, maxon::String::FloatToString(stop.position, -1, 6),
			maxon::String::FloatToString(stop.color.r, -1, 6), maxon::String::FloatToString(stop.color.g, -1, 6),
			maxon::String::FloatToString(stop.color.b, -1, 6), maxon::String::FloatToString(stop.alpha, -1, 6));
	}
	return json + "]}"_s;
}

maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material,
	NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings)
{
	iferr_scope;

	const maxon::Id space(CYCLES_NODESPACE_ID);
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	if (!nodeMaterial->HasSpace(space))
		return false;
	const maxon::nodes::NodesGraphModelRef model = nodeMaterial->GetGraph(space) iferr_return;
	if (!model)
		return false;
	return BuildCyclesNodeSurface(exporter, graph, model, material->GetName(), surface, displacement, settings);
}

maxon::Result<void> WriteCyclesGraphMaterial(MaterialExporter& exporter, const maxon::GraphModelRef& model, const maxon::String& id)
{
	iferr_scope;

	NodeGraph graph;
	const maxon::Int output = graph.Add("ShaderNodeOutputMaterial") iferr_return;
	NodeOutput surface;
	NodeOutput displacement;
	CyclesMaterialSettings settings;
	BuildCyclesNodeSurface(exporter, graph, model, id, surface, displacement, settings) iferr_return;
	if (!surface.IsValid())
	{
		// Only a displacement is wired in, or the graph is empty: the surface shows nothing.
		const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
		surface = NodeOutput { transparent, "BSDF"_s };
	}
	graph.Link(surface, output, "Surface") iferr_return;
	if (displacement.IsValid())
	{
		graph.Link(displacement, output, "Displacement") iferr_return;
	}

	JsonWriter& json = exporter.GetWriter().BeginItem("material", id);
	graph.Write(json);
	if (!settings.names.IsEmpty())
	{
		json.BeginObject("props");
		for (maxon::Int i = 0; i < settings.names.GetCount(); ++i)
			json.Raw(settings.names[i], settings.values[i]);
		json.EndObject();
	}
	exporter.GetWriter().EndItem();
	return maxon::OK;
}

maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, const maxon::GraphModelRef& model,
	const maxon::String& materialName, NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings)
{
	iferr_scope;

	// Every node first, so that wires can refer to nodes that come later.
	maxon::BaseArray<PlacedNode> placed;
	maxon::HashMap<maxon::String, maxon::Int> byName;
	const maxon::GraphNode root = model.GetViewRoot();
	root.GetInnerNodes(maxon::NODE_KIND::NODE, false, [&](const maxon::GraphNode& node) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			PlacedNode entry;
			entry.node = node;
			entry.definition = DefinitionOf(node) iferr_return;
			if (!entry.definition)
				return true;
			if (maxon::CString(entry.definition->asset) != maxon::CString(CYCLES_OUTPUT_NODE_ID))
			{
				entry.index = graph.Add(entry.definition->type) iferr_return;
			}
			byName.Insert(node.GetId().ToString(), placed.GetCount()) iferr_return;
			placed.Append(std::move(entry)) iferr_return;
			return true;
		}) iferr_return;
	if (placed.IsEmpty())
		return false;

	// The source of what is wired into an input, as an output of the Blender graph.
	auto source = [&](const maxon::GraphNode& port) -> maxon::Result<NodeOutput>
	{
		iferr_scope;
		const maxon::GraphNode output = WiredOutput(port) iferr_return;
		if (!output.IsValid())
			return NodeOutput();
		const maxon::GraphNode owner = output.GetAncestor(maxon::NODE_KIND::NODE) iferr_return;
		const auto* const known = owner.IsValid() ? byName.Find(owner.GetId().ToString()) : nullptr;
		if (!known)
			return NodeOutput();
		const PlacedNode& from = placed[known->GetValue()];
		if (from.index == NOTOK)
			return NodeOutput();
		const maxon::CString portName = output.GetId().ToCString();
		for (maxon::Int i = 0; i < from.definition->outputCount; ++i)
		{
			if (portName == maxon::CString(from.definition->outputs[i].port))
				return NodeOutput { from.index, maxon::String(from.definition->outputs[i].identifier) };
		}
		return NodeOutput();
	};

	maxon::Bool hasOutput = false;
	for (const PlacedNode& entry : placed)
	{
		const maxon::GraphNode inputs = entry.node.GetInputs() iferr_return;
		const CyclesNode& definition = *entry.definition;

		if (entry.index == NOTOK)
		{
			// The Material Output: hand back what is wired into it. The first one counts.
			if (hasOutput)
				continue;
			hasOutput = true;
			const maxon::GraphNode surfacePort = inputs.FindChild(maxon::Id("in.surface")) iferr_return;
			if (surfacePort.IsValid())
			{
				surface = source(surfacePort) iferr_return;
			}
			const maxon::GraphNode displacementPort = inputs.FindChild(maxon::Id("in.displacement")) iferr_return;
			if (displacementPort.IsValid())
			{
				displacement = source(displacementPort) iferr_return;
			}
			// Blender's settings of the material itself are settings of this node here.
			for (maxon::Int i = 0; i < definition.propertyCount; ++i)
			{
				const CyclesProperty& property = definition.properties[i];
				const maxon::String name(property.identifier);
				if (!name.StartsWith("material:"_s))
					continue;
				const maxon::Id id = PortId(property.port) iferr_return;
				const maxon::GraphNode port = inputs.FindChild(id) iferr_return;
				if (!port.IsValid())
					continue;
				const maxon::String value = PropertyJson(port, property) iferr_return;
				settings.names.Append(property.identifier + 9) iferr_return;   // without "material:"
				settings.values.Append(value) iferr_return;
			}
			const maxon::GraphNode volumePort = inputs.FindChild(maxon::Id("in.volume")) iferr_return;
			if (volumePort.IsValid())
			{
				const NodeOutput volume = source(volumePort) iferr_return;
				if (volume.IsValid())
					exporter.Warn(FormatString("Material \"@\": what is wired into the Volume input of the Material Output is not rendered yet.", materialName));
			}
			continue;
		}

		for (maxon::Int i = 0; i < definition.inputCount; ++i)
		{
			const CyclesSocket& socket = definition.inputs[i];
			const maxon::Id id = PortId(socket.port) iferr_return;
			const maxon::GraphNode port = inputs.FindChild(id) iferr_return;
			if (!port.IsValid())
				continue;
			const NodeOutput from = source(port) iferr_return;
			if (from.IsValid())
			{
				graph.Link(from, entry.index, socket.identifier) iferr_return;
				continue;
			}
			switch (socket.kind)
			{
				case 'f':
				{
					const maxon::Float value = port.GetEffectivePortValue<maxon::Float>().GetValueOr(maxon::Float(socket.value[0])) iferr_return;
					graph.SetInput(entry.index, socket.identifier, maxon::Float64(value));
					break;
				}
				case 'i':
				{
					const maxon::Int value = port.GetEffectivePortValue<maxon::Int>().GetValueOr(maxon::Int(socket.value[0])) iferr_return;
					graph.SetInput(entry.index, socket.identifier, maxon::Float64(value));
					break;
				}
				case 'b':
				{
					const maxon::Bool value = port.GetEffectivePortValue<maxon::Bool>().GetValueOr(socket.value[0] != 0.0) iferr_return;
					graph.SetInputJson(entry.index, socket.identifier, value ? "true"_s : "false"_s);
					break;
				}
				case 'c':
				{
					const maxon::Color value = port.GetEffectivePortValue<maxon::Color>().GetValueOr(maxon::Color(socket.value[0], socket.value[1], socket.value[2])) iferr_return;
					graph.SetInput(entry.index, socket.identifier, cinema::Vector(value.r, value.g, value.b));
					break;
				}
				case 'v':
				{
					const maxon::Vector value = port.GetEffectivePortValue<maxon::Vector>().GetValueOr(maxon::Vector(socket.value[0], socket.value[1], socket.value[2])) iferr_return;
					graph.SetInput(entry.index, socket.identifier, cinema::Vector(value.x, value.y, value.z));
					break;
				}
				case 't':
				{
					const maxon::String value = port.GetEffectivePortValue<maxon::String>().GetValueOr(maxon::String(socket.text)) iferr_return;
					graph.SetInputJson(entry.index, socket.identifier, "\""_s + value + "\""_s);
					break;
				}
				default: break;   // a shader input with nothing wired in
			}
		}

		// The settings on the node.
		maxon::String imagePath;
		maxon::Bool imageIsColor = true;
		maxon::Bool hasImage = false;
		maxon::Bool hasRamp = false;
		maxon::Bool hasCurves = false;
		for (maxon::Int i = 0; i < definition.propertyCount; ++i)
		{
			const CyclesProperty& property = definition.properties[i];
			const maxon::String name(property.identifier);
			if (name.StartsWith("ramp:"_s))
			{
				hasRamp = true;   // all of them together, below
				continue;
			}
			if (name.StartsWith("curve:"_s))
			{
				hasCurves = true;   // all of them together, below
				continue;
			}
			const maxon::Id id = PortId(property.port) iferr_return;
			const maxon::GraphNode port = inputs.FindChild(id) iferr_return;
			if (!port.IsValid())
				continue;
			if (property.kind == 'u')
			{
				hasImage = true;
				ifnoerr (const maxon::Opt<const maxon::Url&> url = port.GetEffectivePortValue<maxon::Url>())
				{
					if (url && url->IsPopulated())
					{
						imagePath = url->GetSystemPath() iferr_return;
					}
				}
				continue;
			}
			const maxon::String value = PropertyJson(port, property) iferr_return;
			if (maxon::CString(property.identifier) == "image.colorspace"_cs)
				imageIsColor = value != "\"DATA\""_s;
			else if (name.StartsWith("output:"_s))
				graph.SetOutputJson(entry.index, property.identifier + 7, value);   // without "output:"
			else
				graph.SetPropertyJson(entry.index, property.identifier, value);
		}
		if (hasRamp)
		{
			const maxon::String ramp = ColorRampJson(inputs, definition) iferr_return;
			graph.SetPropertyJson(entry.index, "color_ramp", ramp);
		}
		if (hasCurves)
		{
			// The curves of RGB Curves, Vector Curves and Float Curve, sampled from Cinema 4D's
			// splines (cycles_curves.h). Their values are no colours: they go as they are, whatever
			// the document's colour management.
			maxon::BaseArray<maxon::String> problems;
			const maxon::String mapping = CurveMappingJson(inputs, definition, problems) iferr_return;
			graph.SetPropertyJson(entry.index, "mapping", mapping);
			for (const maxon::String& problem : problems)
				exporter.Warn(FormatString("Material \"@\": @ node, @.", materialName, maxon::String(definition.label), problem));
		}
		if (hasImage)
		{
			if (imagePath.IsEmpty())
			{
				exporter.Warn(FormatString("Material \"@\": an @ node names no file.", materialName, maxon::String(definition.label)));
			}
			else
			{
				const maxon::String image = exporter.GetFileImageId(imagePath, imageIsColor) iferr_return;
				graph.SetProperty(entry.index, "image", image);
			}
		}
	}

	if (!surface.IsValid() && !displacement.IsValid())
	{
		exporter.Warn(FormatString("Material \"@\": nothing is wired into its Material Output.", materialName));
		// A material that shows nothing, as in Blender.
		const maxon::Int transparent = graph.Add("ShaderNodeBsdfTransparent") iferr_return;
		surface = NodeOutput { transparent, "BSDF"_s };
	}
	return true;
}

} // namespace cyclesrender
