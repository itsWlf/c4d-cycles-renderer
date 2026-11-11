// SPDX-License-Identifier: Apache-2.0
// The "Cycles Material" command: a new material of Cycles nodes.

#ifndef CYCLESRENDER_CYCLES_MATERIAL_H__
#define CYCLESRENDER_CYCLES_MATERIAL_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Registers the command "Cycles Material" with Cinema 4D. Called once from PluginStart().
///
/// The command makes a node material with a graph in the Cycles node space (a Principled BSDF
/// wired into a Material Output), whatever renderer is chosen, and selects it.
///
/// Why a command and not a material type of its own (which was tried first):
/// Cinema 4D attaches a node graph only to its own material type; for any other type
/// NodeMaterial::CreateDefaultGraph fails with "baseMaterial.IsInstanceOf(Mmaterial) not
/// fulfilled". The settings Blender keeps on a material (Eevee's render method and so on) are
/// therefore settings of the Material Output node (tools/make_nodes.py, MATERIAL_SETTINGS).
//----------------------------------------------------------------------------------------
cinema::Bool RegisterCyclesMaterial();

} // namespace cyclesrender

#endif // CYCLESRENDER_CYCLES_MATERIAL_H__
