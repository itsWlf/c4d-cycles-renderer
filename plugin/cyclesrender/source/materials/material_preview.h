// SPDX-License-Identifier: Apache-2.0
// The picture of a material of Cycles nodes that Cinema 4D shows as its thumbnail: a ball
// wearing the material, rendered by Blender.
//
// Cinema 4D cannot compute Cycles nodes, so it asks the node space for the picture
// (nodespace/cycles_nodespace.cpp, the preview provider). The pictures come from a Blender of
// their own: a background Blender that is started with the first preview and stays until the
// plugin is unloaded, apart from the Blenders that render frames and the IPR. It is sent the
// material as a render is, and asked for `material_preview` (protocol.md).

#ifndef CYCLESRENDER_MATERIAL_PREVIEW_H__
#define CYCLESRENDER_MATERIAL_PREVIEW_H__

#include "c4d.h"
#include "maxon/graph.h"

#include "../scene/scene_writer.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// One preview. Prepare() reads the graph and must be called while the graph may be read;
/// Render() may then run on any thread, later, and blocks until Blender has answered.
//----------------------------------------------------------------------------------------
class MaterialPreview
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(MaterialPreview);

public:
	MaterialPreview() : _writer(false) { }

	/// Writes the material of @p graph (a graph of the Cycles node space) for Blender.
	maxon::Result<void> Prepare(const maxon::GraphModelRef& graph);

	//----------------------------------------------------------------------------------------
	/// Renders the ball. Call once, after Prepare().
	/// @param[in] size     Side of the square picture in pixels.
	/// @param[out] pixels  RGBA, top row first, linear values; size * size * 4 numbers.
	//----------------------------------------------------------------------------------------
	maxon::Result<void> Render(maxon::Int size, maxon::BaseArray<maxon::Float32>& pixels);

private:
	SceneWriter _writer;
	maxon::Bool _prepared = false;
};

/// Ends the Blender that renders previews. Call when the plugin is unloaded.
void FreeMaterialPreviews();

} // namespace cyclesrender

#endif // CYCLESRENDER_MATERIAL_PREVIEW_H__
