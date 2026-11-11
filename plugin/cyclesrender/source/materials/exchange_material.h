// SPDX-License-Identifier: Apache-2.0
// Node materials and Redshift materials, through Cinema 4D's own material exchange.

#ifndef CYCLESRENDER_EXCHANGE_MATERIAL_H__
#define CYCLESRENDER_EXCHANGE_MATERIAL_H__

#include "c4d.h"

#include "node_graph.h"

namespace cyclesrender
{

class MaterialExporter;

//----------------------------------------------------------------------------------------
/// Cinema 4D can describe any of its materials as an Autodesk Standard Surface: a fixed list
/// of numbers and colours, each optionally with a texture (maxon::material::
/// MaterialExportInterface, the same thing its FBX and USD exporters use). This builds a
/// Principled BSDF from that description. It is an approximation by nature: whatever a node
/// graph computes is reduced to that list, and Cinema 4D bakes what does not fit.
///
/// Used: base colour and weight, metalness, specular weight, roughness and IOR,
/// transmission, coat (weight, roughness, IOR), sheen, emission, opacity. Textures are used
/// for base colour, roughness, metalness, emission colour and opacity.
///
/// @param[out] surface  The surface output of the nodes added to @p graph.
/// @return              False, with nothing added, when Cinema 4D could not describe the material.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> BuildExchangeSurface(MaterialExporter& exporter, NodeGraph& graph, cinema::BaseMaterial* material, NodeOutput& surface);

} // namespace cyclesrender

#endif // CYCLESRENDER_EXCHANGE_MATERIAL_H__
