// SPDX-License-Identifier: Apache-2.0
// The effects of a camera's Cycles Camera tag (its Imager tab), with the camera's exposure before them
// (the tag's Color tab and a Redshift camera's own), as a `compositor` item: a node graph in Blender's
// own names, as materials are sent (protocol/protocol.md, "The compositor").

#ifndef CYCLESRENDER_IMAGER_EXPORT_H__
#define CYCLESRENDER_IMAGER_EXPORT_H__

#include "c4d.h"

#include "../tags/imager_effects.h"
#include "document_colors.h"
#include "redshift_camera.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The id of the one `compositor` item: the camera that renders decides whose effects it holds.
maxon::String ImagerItemId();

//----------------------------------------------------------------------------------------
/// What the exporter reads of a camera's picture: the exposure applied first (the Color tab of its
/// Cycles Camera tag and, for a Redshift camera, Redshift's own exposure: their stops add, so their
/// factors multiply), then the Imager tab's layers that are switched on, in the order they are
/// applied (the list's bottom layer first, its top layer last, as Cinema 4D's field lists apply
/// theirs). Kept apart from the tag, like CameraValues, so that the IPR can go on rendering through
/// a camera as it was (its camera lock).
//----------------------------------------------------------------------------------------
struct ImagerChain
{
	struct Layer
	{
		const ImagerEffect* effect = nullptr;	///< never nullptr in a chain that was read
		maxon::String name;										///< the layer's name, for the warnings
		maxon::Float64 opacity = 1.0;					///< 0 to 1
		maxon::Int firstValue = 0;						///< its values in `values`: one per setting of the effect, in the table's order
		maxon::Int32 engine = 0;							///< IMAGERLAYER_ENGINE_BOTH, _CYCLES or _EEVEE: where it applies
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
	/// The camera's exposure in stops, applied to the scene-linear picture before the layers and before
	/// the view: the Color tab's (CYCLESCAMERA_COLOR_EXPOSURE) plus a Redshift camera's (ReadRedshiftExposure).
	/// It does not depend on the Imager tab's switch.
	maxon::Float64 exposure = 0.0;
	/// What of the camera's picture settings is not translated (a Redshift camera's tone mapping): a warning each.
	maxon::BaseArray<maxon::String> notes;

	maxon::Bool IsEmpty() const { return layers.IsEmpty() && unknown.IsEmpty() && exposure == 0.0 && notes.IsEmpty(); }
	maxon::Result<void> CopyFrom(const ImagerChain& other);

	/// One value of a layer the plugin makes itself (a translated effect), by the setting's identifier.
	struct Given
	{
		const maxon::Char* identifier;
		maxon::Float64 number[3];
		const maxon::Char* option;			///< a menu's option, Blender's name; nullptr for a number
	};

	/// Appends a layer of the table's effect @p effectName of source @p source (IMAGERLAYER_SOURCE_...), applied
	/// in both engines at full opacity: the table's defaults, and @p given for the settings it names. Does nothing
	/// when the table has no such effect (an error of the plugin's own: none is ever asked for that is not there).
	maxon::Result<void> AppendLayer(const maxon::Char* effectName, maxon::Int32 source, const maxon::String& name,
		std::initializer_list<Given> given);

	/// Reads the exposure and the Imager tab of @p camera. Empty for a camera without exposure and
	/// without a layer that is switched on (no Cycles Camera tag, or its Imager tab switched off).
	/// What Redshift does to the picture of this camera is translated first, as far as @p redshift says (the
	/// Render Settings' "Scene Translation" > "Redshift"; all of it by default): its exposure, and as layers
	/// before the tag's own (scene/redshift_camera.h, ReadRedshiftEffects).
	static maxon::Result<ImagerChain> Read(const cinema::BaseObject* camera, const RedshiftTranslation& redshift = RedshiftTranslation());
};

/// What renders the picture the `compositor` item is for.
struct ImagerTarget
{
	maxon::Int32 blenderVersion = 0;	///< the Blender in use, a BlenderVersionCode()
	maxon::Bool eevee = false;				///< Eevee renders (else Cycles): a layer for the other engine is left out
	maxon::Bool viewport = false;			///< the IPR's viewport (else a final frame): what its compositor is given differs
};

//----------------------------------------------------------------------------------------
/// Writes @p chain as the `compositor` item: the rendered picture, then the camera's exposure (an
/// Exposure node, there at 0 stops too, so that another exposure is another value and never another
/// graph), then per layer the effect and a Mix by the layer's opacity, then the output.
/// A layer for the other engine than @p target's is left out, as if it were switched off.
/// What the Blender in use (@p target's blenderVersion) does not have is left out and
/// named in a warning: an effect, a setting that is not at its default, a menu's option.
/// @return  False when nothing was written (no exposure and no layer this Blender has): the picture
///          is the render.
///          In a request of some items only, the caller then names the item for removal.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportImager(SceneWriter& writer, const ImagerChain& chain, const DocumentColors& colors, const ImagerTarget& target);

} // namespace cyclesrender

#endif // CYCLESRENDER_IMAGER_EXPORT_H__
