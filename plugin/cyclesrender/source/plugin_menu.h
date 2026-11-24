// SPDX-License-Identifier: Apache-2.0
// The plugin's submenu of Cinema 4D's Extensions menu: "Cycles Render" with the IPR, a submenu of
// the material types and a submenu of the objects, in the order of the IPR window's menus.
//
// Cinema 4D lists every command and object of a plugin by itself in a submenu named after the
// plugin's folder ("cyclesrender"), in an order of its own. The plugin's commands and objects are
// therefore registered with PLUGINFLAG_HIDEPLUGINMENU, which leaves them out of that list only
// (docs/reference/c4d-sdk.md, "The plugin's menus"), and this submenu is put into the Extensions
// menu instead when Cinema 4D builds its menus (C4DPL_BUILDMENU).

#ifndef CYCLESRENDER_PLUGIN_MENU_H__
#define CYCLESRENDER_PLUGIN_MENU_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Puts the submenu "Cycles Render" at the end of the Extensions menu, unless a submenu of that
/// title is there already (a menu the user customised and saved with it). Call on C4DPL_BUILDMENU.
/// @return  Whether the submenu was added.
//----------------------------------------------------------------------------------------
cinema::Bool AddPluginMenu();

} // namespace cyclesrender

#endif // CYCLESRENDER_PLUGIN_MENU_H__
