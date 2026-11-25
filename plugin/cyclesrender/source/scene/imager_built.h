// SPDX-License-Identifier: Apache-2.0
// The Imager's effects that the plugin builds of several of Blender's compositor nodes (a layer of the
// source IMAGERLAYER_SOURCE_BUILT, 0.3.0, F13): what each does and the measurements behind it are in
// docs/reference/blender.md, "The compositor" (Edges, Tone Mapping, White Balance, Lens Vignetting, Lens Glare). Their settings are in the generated table like every
// other effect's (tools/make_effects.py, BUILT_EFFECTS); this file turns the values into nodes.

#ifndef CYCLESRENDER_IMAGER_BUILT_H__
#define CYCLESRENDER_IMAGER_BUILT_H__

#include "c4d.h"

#include "../materials/node_graph.h"
#include "imager_export.h"

namespace cyclesrender
{

/// What a built effect is made in: the tree, its Render Layers node (for the passes), and what renders.
struct ImagerBuildContext
{
	NodeGraph& graph;
	maxon::Int render;								///< the Render Layers node
	const DocumentColors& colors;
	const ImagerTarget& target;
	SceneWriter& writer;							///< for the warnings
	/// View layer properties of the passes the tree reads ("use_pass_z"): the `compositor` item's `view_layer`.
	maxon::BaseArray<maxon::String>& passes;
};

/// Adds the nodes of a built effect to the tree, fed by @p picture. @p values are the layer's values,
/// one per setting of @p effect in the table's order.
/// @return  The output that is the effect's picture.
maxon::Result<NodeOutput> BuildImagerEffect(ImagerBuildContext& context, const ImagerEffect& effect, const ImagerChain::Value* values,
	const maxon::String& layerName, const NodeOutput& picture);

} // namespace cyclesrender

#endif // CYCLESRENDER_IMAGER_BUILT_H__
