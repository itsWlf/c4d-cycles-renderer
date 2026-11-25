// SPDX-License-Identifier: Apache-2.0
// The legacy Redshift material "RS Shader Graph" (an Xpresso graph of Redshift shader nodes), read through a Redshift node
// graph made from it.

#ifndef CYCLESRENDER_REDSHIFT_LEGACY_H__
#define CYCLESRENDER_REDSHIFT_LEGACY_H__

#include "c4d.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

//----------------------------------------------------------------------------------------
/// Builds Blender shaders for a legacy "RS Shader Graph" material (REDSHIFT_SHADER_GRAPH_MATERIAL, 0.3.0 F13 W17). Its graph is
/// an Xpresso graph under the material's "Node" branch: Redshift shader nodes (REDSHIFT_GV_SHADER, their "Shader Class" the
/// same name as a node of Redshift's node space, their parameters the same names as its ports), an Output node, and nodes
/// that hold a Cinema 4D shader. The graph is copied into a Redshift node graph of a material made for the purpose (each
/// shader node becomes the node-space node of its class, each parameter the port of its name, each wire the wire between
/// those ports; a Cinema 4D shader wired into a Texture node's file becomes the file of that shader baked) and that material
/// is translated as any Redshift node material (BuildRedshiftSurface, redshift_material.h). What cannot be copied (a node of
/// another kind, a class the node space lacks) is named in the console.
///
/// @param[out] surface  The surface output of the nodes added to @p graph.
/// @return              False, with nothing added, for any other material or when its graph gives no surface.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildRedshiftShaderGraph(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface);

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_LEGACY_H__
