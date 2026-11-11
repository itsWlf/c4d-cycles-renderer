// SPDX-License-Identifier: Apache-2.0
// A material built from the nodes of the "Cycles" node space.

#ifndef CYCLESRENDER_CYCLES_NODE_MATERIAL_H__
#define CYCLESRENDER_CYCLES_NODE_MATERIAL_H__

#include "c4d.h"
#include "maxon/graph.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

/// Blender's settings of the material itself (render method, displacement method, ...), read
/// from the Material Output node: the name of each Blender property and its value as JSON text.
struct CyclesMaterialSettings
{
	maxon::BaseArray<const maxon::Char*> names;
	maxon::BaseArray<maxon::String> values;
};

//----------------------------------------------------------------------------------------
/// Writes the material's Cycles node graph as Blender nodes: one node for each node, with its
/// values, the settings on the node and the wires between nodes (nodespace/cycles_nodes.h has
/// the table both sides share). An Image Texture's file becomes an `image` item.
///
/// The Material Output node itself is not written; what is wired into its inputs is returned,
/// and the caller connects it to the output it owns.
///
/// @param[out] surface       What is wired into the Surface input; invalid if nothing is.
/// @param[out] displacement  The same for Displacement.
/// @param[out] settings      The material settings on the Material Output node, for the item's `props`.
/// @return                   False, with nothing added, when the material has no graph in the Cycles space.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material,
	NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings);

/// The same for a graph that is at hand without its material (a preview is asked for a graph).
/// @param[in] materialName  For the warnings.
maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, const maxon::GraphModelRef& model,
	const maxon::String& materialName, NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings);

/// Writes the whole `material` item (and the `image` items it needs) for a graph of Cycles
/// nodes under the id @p id, through the exporter's writer.
maxon::Result<void> WriteCyclesGraphMaterial(MaterialExporter& exporter, const maxon::GraphModelRef& model, const maxon::String& id);

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_NODE_MATERIAL_H__
