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
/// Builds a Principled BSDF from the Redshift Standard Material node that feeds the
/// material's output, option by option (the table is in docs/reference/c4d-sdk.md,
/// "Redshift Standard Material"). This is the base shader of every new Redshift material,
/// and reading it directly keeps what Cinema 4D's neutral description (exchange_material.h)
/// loses: transmission colour, subsurface, sheen and coat colours, thin film, anisotropy,
/// bump and normal maps.
///
/// An input is a constant, or a Texture node plugged straight into it (through a Bump Map
/// node for the two bump inputs). Any other node plugged into an input is not translated:
/// the user is told, and the input's own value is used.
///
/// @param[out] surface  The surface output of the nodes added to @p graph.
/// @return              False, with nothing added, when the material's surface is not a
///                      Standard Material node; the caller then falls back to the neutral description.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildRedshiftSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface);

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_MATERIAL_H__
