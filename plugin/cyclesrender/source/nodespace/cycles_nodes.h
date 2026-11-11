// SPDX-License-Identifier: Apache-2.0
// Blender's shader nodes as a table: what the "Cycles" node space of Cinema 4D is built from
// and what a graph of its nodes is written back with.

#ifndef CYCLESRENDER_CYCLES_NODES_H__
#define CYCLESRENDER_CYCLES_NODES_H__

#include "c4d.h"
#include "maxon/graph.h"

namespace cyclesrender
{

/// An input or output socket of a Blender node.
struct CyclesSocket
{
	const maxon::Char* identifier;  ///< Blender's identifier of the socket, unique within the node ("Base Color", "Value_001")
	const maxon::Char* port;        ///< id of the port in Cinema 4D ("in.base_color")
	const maxon::Char* label;       ///< name shown on the node
	maxon::Char kind;               ///< 'f' number, 'c' colour, 'v' vector, 's' shader (closure), 't' text, 'b' switch, 'i' whole number
	maxon::Float64 value[4];        ///< default of an input
	const maxon::Char* text;        ///< default of a text input
	/// For which settings of the node Blender shows the socket: "" always, "!" never, or
	/// "prop.data_type=RGBA|ROTATION&prop.clamp=1" (each named setting has one of the listed values).
	const maxon::Char* show;
};

/// A setting on the node itself (not a socket): a dropdown, switch, number, colour, vector or text.
///
/// Most are properties of the Blender node. Three kinds are told by a prefix of the identifier:
/// "material:<property>" is a setting of the Blender material (on the Material Output only),
/// "output:<socket>" is the value a Value, RGB or Normal node keeps on its output socket, and
/// "ramp:<part>" is a part of a Color Ramp's stops (tools/make_nodes.py says why).
struct CyclesProperty
{
	const maxon::Char* identifier;  ///< Blender's name of the property ("distribution"), or prefixed as above
	const maxon::Char* port;        ///< id of the port in Cinema 4D ("prop.distribution")
	const maxon::Char* label;
	maxon::Char kind;               ///< 'e' one of several options, 'b' switch, 'i' whole number, 'f' number, 'c' colour, 'v' vector, 't' text, 'u' file
	const maxon::Char* items;       ///< options as "IDENTIFIER|Label;IDENTIFIER|Label"
	const maxon::Char* text;        ///< default option or text; of a colour or vector, its default as "0.5 0.5 0.5"
	maxon::Float64 number;          ///< default number or switch
};

struct CyclesNode
{
	const maxon::Char* type;        ///< Blender's node type ("ShaderNodeBsdfPrincipled")
	const maxon::Char* asset;       ///< id of the node in Cinema 4D
	const maxon::Char* label;
	const maxon::Char* tag;         ///< Blender's colour tag: SHADER, TEXTURE, INPUT, OUTPUT, COLOR, VECTOR, CONVERTER
	const CyclesSocket* inputs;
	maxon::Int inputCount;
	const CyclesSocket* outputs;
	maxon::Int outputCount;
	const CyclesProperty* properties;
	maxon::Int propertyCount;
};

/// A folder of the nodes in the Asset Browser, after Blender's Add menu.
struct CyclesNodeFolder
{
	const maxon::Char* id;          ///< id of the category asset
	const maxon::Char* label;
	const maxon::Char* parent;      ///< id of the folder it is in, or nullptr for the top one ("Cycles Nodes")
};

/// The id of the node space; also the key of a material's graph in scene files.
#define CYCLES_NODESPACE_ID "com.wolfnimations.cyclesrender.nodespace"
/// The description database of the nodes: res/nodes/<this id>.db.json, written by tools/make_nodes.py.
#define CYCLES_NODES_DATABASE_ID "com.wolfnimations.cyclesrender.nodes"
/// The node every material ends in.
#define CYCLES_OUTPUT_NODE_ID "com.wolfnimations.cyclesrender.node.outputmaterial"
#define CYCLES_PRINCIPLED_NODE_ID "com.wolfnimations.cyclesrender.node.bsdfprincipled"

/// All nodes, generated from Blender by tools/make_nodes.py.
maxon::Block<const CyclesNode> GetCyclesNodes();

/// The folders the nodes are sorted into, parents before their children.
maxon::Block<const CyclesNodeFolder> GetCyclesNodeFolders();

/// Whether Blender shows this socket for the settings the node has now.
maxon::Result<maxon::Bool> IsCyclesSocketShown(const maxon::GraphNode& node, const CyclesNode& definition, const CyclesSocket& socket);

/// The default of a colour or vector setting (kind 'c' or 'v').
maxon::Vector GetCyclesPropertyVector(const CyclesProperty& property);

/// The node with this Cinema 4D id, or nullptr.
const CyclesNode* FindCyclesNode(const maxon::Id& asset);

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_NODES_H__
