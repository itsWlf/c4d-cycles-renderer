// SPDX-License-Identifier: Apache-2.0
// Blender's compositor effects as a table, and the layers of a Cycles Camera tag's Imager tab.
//
// The table is generated (tools/make_effects.py asks every supported Blender for the effects it
// ships and their settings): the plugin holds no knowledge of Blender's effects beyond it.
//
// A layer is one effect in the tag's list, with a name, a switch, an opacity and the effect's
// settings. How layers are stored in the tag is written down in res/description/tcyclescamera.h,
// "The Imager tab": it is stored in scene files and never changes.

#ifndef CYCLESRENDER_IMAGER_EFFECTS_H__
#define CYCLESRENDER_IMAGER_EFFECTS_H__

#include "c4d.h"

namespace cyclesrender
{

/// One setting of an effect: an input socket of Blender's node group that takes a value.
struct ImagerSetting
{
	const maxon::Char* identifier;	///< Blender's identifier of the socket ("Socket_10"): what the bridge is sent
	maxon::Int32 number;						///< the field of a layer its value is stored in (IMAGERLAYER_SETTINGS and up); stored in scene files
	const maxon::Char* label;				///< Blender's name of it
	maxon::Char kind;								///< 'f' number, 'i' whole number, 'b' switch, 'c' colour, 'v' vector, 'm' menu
	maxon::Char subtype;						///< 'F' a factor (a slider between its limits), 'A' an angle in radians, 'N' a plain number
	maxon::Float64 value[4];				///< Blender's default
	maxon::Int32 count;							///< numbers a vector or colour socket takes (2 for a place in the picture)
	maxon::Float64 minimum;					///< Blender's limits; beyond +-1e29 there is none
	maxon::Float64 maximum;
	const maxon::Char* panel;				///< Blender's panel the setting is in; "" for none
	const maxon::Char* options;			///< a menu's options, Blender's names, separated by line feeds
	const maxon::Char* optionSince;	///< the first Blender that has each option, separated by commas
	maxon::Int32 defaultOption;			///< index of a menu's default option
	const maxon::Char* help;				///< Blender's description of it
	maxon::Int32 since;							///< the first Blender that has it, as a BlenderVersionCode() (503 for 5.3)
};

/// One effect: a compositor node group of Blender's bundled asset library that takes a picture and gives one.
struct ImagerEffect
{
	const maxon::Char* name;				///< Blender's name of the group: what a layer stores and the bridge is sent
	const maxon::Char* help;				///< Blender's description of it
	const maxon::Char* catalog;			///< Blender's asset catalog of it
	const maxon::Char* imageIn;			///< identifier of the socket the picture goes in by
	const maxon::Char* imageOut;		///< identifier of the socket it comes out by
	const ImagerSetting* settings;
	maxon::Int32 settingCount;
	maxon::Int32 since;							///< the first Blender that has it
};

/// Every effect of the table, by name.
maxon::Block<const ImagerEffect> GetImagerEffects();

/// The effect of Blender's name @p name, or nullptr.
const ImagerEffect* FindImagerEffect(const maxon::String& name);

/// A menu setting's options: their number, Blender's name of one and the first Blender that has it.
maxon::Int32 GetImagerOptionCount(const ImagerSetting& setting);
maxon::String GetImagerOption(const ImagerSetting& setting, maxon::Int32 index);
maxon::Int32 GetImagerOptionSince(const ImagerSetting& setting, maxon::Int32 index);
/// The number a layer stores for option @p index of a menu: made from the option's name, so it stays
/// what it is when Blender adds options or changes their order (stored in scene files).
maxon::Int32 GetImagerOptionValue(const ImagerSetting& setting, maxon::Int32 index);
/// The index of the option a stored number stands for; NOTOK when the menu has none for it.
maxon::Int32 FindImagerOption(const ImagerSetting& setting, maxon::Int32 value);

//----------------------------------------------------------------------------------------
// The layers of a tag (its container @p data). The order is the list's, the top layer first; the
// top layer is applied last, as in Cinema 4D's field lists.
//----------------------------------------------------------------------------------------

/// The id in the tag's container of field @p field of layer @p layer (res/description/tcyclescamera.h).
maxon::Int32 ImagerLayerId(maxon::Int32 layer, maxon::Int32 field);

/// The layer and the field of an id of the tag's container; false when @p id is no layer's.
maxon::Bool ImagerLayerOfId(maxon::Int32 id, maxon::Int32& layer, maxon::Int32& field);

/// The numbers of the tag's layers, the top one first.
maxon::Result<void> GetImagerLayers(const cinema::BaseContainer& data, maxon::BaseArray<maxon::Int32>& layers);

/// Puts the layers into the order @p layers (which must name layers the tag has; others are dropped
/// with their values).
void SetImagerLayers(cinema::BaseContainer& data, const maxon::BaseArray<maxon::Int32>& layers);

/// The effect of a layer, or nullptr when the table has none of its name (a scene of a newer plugin).
const ImagerEffect* GetImagerLayerEffect(const cinema::BaseContainer& data, maxon::Int32 layer);

/// Adds a layer of @p effect on top of the list, with Blender's defaults, and selects it.
/// @return  The new layer's number, or 0.
maxon::Int32 AddImagerLayer(cinema::BaseContainer& data, const ImagerEffect& effect);

/// Removes a layer with everything it stores. The layer next to it becomes the selected one.
void RemoveImagerLayer(cinema::BaseContainer& data, maxon::Int32 layer);

/// Moves a layer one place up (@p direction -1) or down (1) in the list. False at the list's end.
maxon::Bool MoveImagerLayer(cinema::BaseContainer& data, maxon::Int32 layer, maxon::Int32 direction);

/// The layer whose settings the Attribute Manager shows, or 0. Always one of the tag's layers.
maxon::Int32 GetSelectedImagerLayer(const cinema::BaseContainer& data);

/// The default of a setting as the tag stores it (a menu: the option's name).
cinema::GeData GetImagerDefault(const ImagerSetting& setting);

} // namespace cyclesrender

#endif // CYCLESRENDER_IMAGER_EFFECTS_H__
