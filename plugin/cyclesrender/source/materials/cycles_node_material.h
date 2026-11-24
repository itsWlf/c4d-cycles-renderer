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

/// What the World Output node of a world material (the Cycles Environment's Material type) has wired in.
struct CyclesWorldOutputs
{
	maxon::Bool found = false;     ///< the graph has a World Output
	NodeOutput surface;            ///< wired into its Surface; invalid if nothing is
	NodeOutput volume;             ///< into its Volume
};

//----------------------------------------------------------------------------------------
/// Writes the material's Cycles node graph as Blender nodes: one node for each node, with its
/// values, the settings on the node and the wires between nodes (nodespace/cycles_nodes.h has
/// the table both sides share). An Image Texture's file becomes an `image` item.
///
/// The Material Output node itself is not written; what is wired into its inputs is returned,
/// and the caller connects it to the output it owns. The same for a World Output (a world material):
/// not written, and what is wired into it handed back through @p world. Of several outputs of a kind
/// the one Blender would use for the engine counts (its Target, then "Active Output", then the first;
/// MaterialExporter::IsEevee). With @p world (a Cycles Environment) the Material Outputs are not read;
/// without it (the material is on an object) the World Outputs are not, and a material that has only a
/// wired World Output, nothing wired into a Material Output, is named in the console, as it shows nothing there.
///
/// @param[out] surface       What is wired into the Surface input; invalid if nothing is.
/// @param[out] displacement  The same for Displacement.
/// @param[out] settings      The material settings on the Material Output node, for the item's `props`.
/// @param[out] world         If given: the World Output's inputs.
/// @return                   False, with nothing added, when the material has no graph in the Cycles space.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material,
	NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings, CyclesWorldOutputs* world = nullptr);

/// The same for a graph that is at hand without its material (a preview is asked for a graph).
/// @param[in] materialName  For the warnings.
/// @param[in] material      The material the graph belongs to, which keeps the links of settings that name an
///                          object of the scene (nodespace/object_link.h); without it such a setting is written
///                          as if it named none.
maxon::Result<maxon::Bool> BuildCyclesNodeSurface(MaterialExporter& exporter, NodeGraph& graph, const maxon::GraphModelRef& model,
	const maxon::String& materialName, NodeOutput& surface, NodeOutput& displacement, CyclesMaterialSettings& settings, CyclesWorldOutputs* world = nullptr,
	const cinema::BaseMaterial* material = nullptr);

/// Whether @p material is a world material: a material of Cycles nodes whose graph has a World Output node.
maxon::Bool IsCyclesWorldMaterial(const cinema::BaseMaterial* material);

/// Whether @p material is a material of Cycles nodes (a graph in the Cycles node space).
maxon::Bool IsCyclesNodeMaterial(const cinema::BaseMaterial* material);

/// Writes the whole `material` item (and the `image` items it needs) for a graph of Cycles
/// nodes under the id @p id, through the exporter's writer.
maxon::Result<void> WriteCyclesGraphMaterial(MaterialExporter& exporter, const maxon::GraphModelRef& model, const maxon::String& id);

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_NODE_MATERIAL_H__
