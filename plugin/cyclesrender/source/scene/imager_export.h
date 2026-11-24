// SPDX-License-Identifier: Apache-2.0
// The effects of a camera's Cycles Camera tag (its Imager tab) as a `compositor` item: a node graph
// in Blender's own names, as materials are sent (protocol/protocol.md, "The compositor").

#ifndef CYCLESRENDER_IMAGER_EXPORT_H__
#define CYCLESRENDER_IMAGER_EXPORT_H__

#include "c4d.h"

#include "../tags/imager_effects.h"
#include "document_colors.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The id of the one `compositor` item: the camera that renders decides whose effects it holds.
maxon::String ImagerItemId();

//----------------------------------------------------------------------------------------
/// What the exporter reads of a camera's Imager tab: its layers that are switched on, in the
/// order they are applied (the list's bottom layer first, its top layer last, as Cinema 4D's
/// field lists apply theirs). Kept apart from the tag, like CameraValues, so that the IPR can go on
/// rendering through a camera as it was (its camera lock).
//----------------------------------------------------------------------------------------
struct ImagerChain
{
	struct Layer
	{
		const ImagerEffect* effect = nullptr;	///< never nullptr in a chain that was read
		maxon::String name;										///< the layer's name, for the warnings
		maxon::Float64 opacity = 1.0;					///< 0 to 1
		maxon::Int firstValue = 0;						///< its values in `values`: one per setting of the effect, in the table's order
	};

	struct Value
	{
		maxon::Float64 number[3] = { 0.0, 0.0, 0.0 };	///< a number, a switch (0 or 1), a colour or a vector
		maxon::Int32 option = 0;											///< a menu: the index of the chosen option in the table
	};

	maxon::BaseArray<Layer> layers;
	maxon::BaseArray<Value> values;
	/// Layers the plugin cannot send (an effect this version of the plugin does not know): the names, for a warning each.
	maxon::BaseArray<maxon::String> unknown;

	maxon::Bool IsEmpty() const { return layers.IsEmpty() && unknown.IsEmpty(); }
	maxon::Result<void> CopyFrom(const ImagerChain& other);

	/// Reads the Imager tab of the Cycles Camera tag of @p camera. Empty for a camera without the
	/// tag, with the tab switched off, or without a layer that is switched on.
	static maxon::Result<ImagerChain> Read(const cinema::BaseObject* camera);
};

//----------------------------------------------------------------------------------------
/// Writes @p chain as the `compositor` item: the rendered picture, then per layer the effect and a
/// Mix by the layer's opacity, then the output.
/// What the Blender in use (@p blenderVersion, a BlenderVersionCode()) does not have is left out and
/// named in a warning: an effect, a setting that is not at its default, a menu's option.
/// @return  False when nothing was written (no layer this Blender has): the picture is the render.
///          In a request of some items only, the caller then names the item for removal.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportImager(SceneWriter& writer, const ImagerChain& chain, const DocumentColors& colors, maxon::Int32 blenderVersion);

} // namespace cyclesrender

#endif // CYCLESRENDER_IMAGER_EXPORT_H__
