// SPDX-License-Identifier: Apache-2.0
// A Redshift material read from its own node graph.

#ifndef CYCLESRENDER_REDSHIFT_MATERIAL_H__
#define CYCLESRENDER_REDSHIFT_MATERIAL_H__

#include "c4d.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

//----------------------------------------------------------------------------------------
/// Builds Blender shaders from the Redshift material node that feeds the material's output,
/// option by option (the tables are in docs/reference/c4d-sdk.md, "Redshift Standard
/// Material" and "Redshift nodes"): the Standard Material and the OpenPBR Material as
/// Principled BSDFs, Incandescent as an Emission shader, a Sprite as a cut-out of the material
/// wired into it, and a Store Color/Scalar/Integer To AOV node as the material wired into its
/// beauty input. Reading them directly keeps what Cinema 4D's neutral description
/// (exchange_material.h) loses: transmission colour, subsurface, sheen and coat colours, thin
/// film, anisotropy, bump and normal maps, and the nodes wired into the inputs.
///
/// An input is a constant, or a chain of the nodes redshift_nodes.h translates (through a Bump
/// Map, the old Normal Map, Round Corners or a Bump Blender for the two bump inputs). Any other
/// node plugged into an input is not translated: the user is told, and the input's own value is used.
///
/// @param[out] surface  The surface output of the nodes added to @p graph.
/// @return              False, with nothing added, when the material's surface is none of these
///                      nodes; the caller then falls back to the neutral description.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildRedshiftSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface);

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_MATERIAL_H__
