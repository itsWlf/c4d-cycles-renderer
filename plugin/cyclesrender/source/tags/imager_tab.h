// SPDX-License-Identifier: Apache-2.0
// The Imager tab of the Cycles Camera tag: what the Attribute Manager shows of the tag's layers
// (imager_effects.h) and what its menu and buttons do. tags/camera_tag.cpp calls these from the
// tag's own hooks.
//
// The tab, from top to bottom: the switch "Imager"; the menu "Add Effect", which adds a layer of the
// chosen effect on top; the list of layers; and the selected layer's settings: its name, its switch,
// its opacity and the effect's own settings as Blender names them, in Blender's panels.
//
// The list is an item tree like the field list's where the SDK offers one (sdk_compat.h,
// CYCLESRENDER_ITEM_TREE): a checkbox in front of each row switches the layer, a click selects it, a
// drag moves it, a double click renames it, the Delete key removes it, a slider sets its opacity.
// Elsewhere it is the menu "Layer" and the buttons Remove, Move Up and Move Down, which act on the
// selected layer; where the item tree is, these are hidden but still answer a script.

#ifndef CYCLESRENDER_IMAGER_TAB_H__
#define CYCLESRENDER_IMAGER_TAB_H__

#include "c4d.h"

namespace cyclesrender
{

/// The tab's defaults in a new tag: switched on, no layers.
void InitImagerTab(cinema::BaseContainer& data);

/// For GetDDescription, after the resource was loaded: fills the menus and adds every layer's
/// settings; only the selected layer's are shown.
maxon::Bool BuildImagerDescription(const cinema::BaseList2D* tag, cinema::Description* description);

/// For GetDEnabling: false for what is greyed out (everything while the tab's switch is off, a
/// button that can do nothing).
maxon::Bool IsImagerParameterEnabled(const cinema::BaseContainer& data, maxon::Int32 id);

/// For SetDParameter: a value set on the menu "Add Effect" adds a layer instead of being stored.
/// @return  True when @p id was the menu's (nothing is to be stored then).
maxon::Bool SetImagerMenu(cinema::BaseList2D* tag, maxon::Int32 id, const cinema::GeData& value);

/// For MSG_DESCRIPTION_COMMAND: the buttons. One undo step each. True when @p id was a button's.
maxon::Bool RunImagerCommand(cinema::BaseList2D* tag, maxon::Int32 id);

/// For MSG_ITEMTREE_CALLBACK (where the SDK has the list: sdk_compat.h, CYCLESRENDER_ITEM_TREE): what the
/// user did in the list of layers. An entry selected, switched, renamed or dragged is taken into the
/// layers; the Delete key removes the selected entries' layers; the opacity column's slider sets the
/// layer's opacity. (docs/reference/c4d-sdk.md has the order in which the list's messages come.)
/// @return  True when the message was answered here.
maxon::Bool HandleImagerList(cinema::BaseList2D* tag, void* message);

/// For SetDParameter, after the value was stored: the list itself was set (the Attribute Manager writes
/// it back when the user changed it): the layers take its switches, its order and its selected entry
/// where they differ. True when @p id was the list's.
maxon::Bool SetImagerList(cinema::BaseList2D* tag, maxon::Int32 id);

/// For MSG_DESCRIPTION_POSTSETPARAMETER: a value was set that changes what the tab shows (the
/// selected layer, a layer's name or switch): has the Attribute Manager build the tab again.
void ImagerParameterWasSet(cinema::BaseList2D* tag, maxon::Int32 id);

} // namespace cyclesrender

#endif // CYCLESRENDER_IMAGER_TAB_H__
