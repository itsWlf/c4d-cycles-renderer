// SPDX-License-Identifier: Apache-2.0
// The plugin's submenu of the Extensions menu (plugin_menu.h).

#include "c4d.h"
#include "c4d_resource.h"
#include "gui.h"
#include "c4d_symbols.h"

#include "ids.h"
#include "plugin_menu.h"

namespace cyclesrender
{

// A menu entry that runs the command or makes the object registered under @p id; Cinema 4D shows
// its registered name and icon, and leaves the entry out when no plugin of that id is there.
static void AddCommand(cinema::BaseContainer& menu, cinema::Int32 id)
{
	menu.InsData(cinema::MENURESOURCE_COMMAND, FormatString("PLUGIN_CMD_@", id));
}

cinema::Bool AddPluginMenu()
{
	// Cinema 4D's own menu, which a plugin extends while the menus are built (the SDK's way: the
	// GeData it hands out is const, the container in it is Cinema 4D's and meant to be changed).
	const cinema::GeData* const found = cinema::SearchPluginMenuResource("IDS_EDITOR_PIPELINE"_s);
	cinema::BaseContainer* const extensions = found ? const_cast<cinema::BaseContainer*>(found->GetContainer()) : nullptr;
	if (!extensions)
		return false;
	const maxon::String title = cinema::GeLoadString(IDS_CYCLESRENDER_NAME);
	for (cinema::Int32 index = 0; extensions->GetIndexId(index) != NOTOK; ++index)
	{
		const cinema::GeData* const value = extensions->GetIndexData(index);
		if (extensions->GetIndexId(index) == cinema::MENURESOURCE_SUBMENU && value && value->GetContainer()
			&& value->GetContainer()->GetString(cinema::MENURESOURCE_SUBTITLE) == title)
			return false;
	}

	cinema::BaseContainer materials;
	materials.InsData(cinema::MENURESOURCE_SUBTITLE, cinema::GeLoadString(IDS_MENU_MATERIALS));
	AddCommand(materials, ID_CYCLES_MATERIAL);
	AddCommand(materials, ID_CYCLES_WORLD_MATERIAL);
	// Only there while it is switched on for development (materials/native_material.h).
	if (cinema::FindPlugin(ID_NATIVE_MATERIAL, cinema::PLUGINTYPE::COMMAND))
		AddCommand(materials, ID_NATIVE_MATERIAL);

	// In the order of the IPR window's menu "Objects" (ipr/ipr_dialog.cpp).
	cinema::BaseContainer objects;
	objects.InsData(cinema::MENURESOURCE_SUBTITLE, cinema::GeLoadString(IDS_MENU_OBJECTS));
	AddCommand(objects, ID_CAMERA_COMMAND);
	AddCommand(objects, ID_LIGHT_OBJECT);
	AddCommand(objects, ID_ENVIRONMENT_OBJECT);
	AddCommand(objects, ID_FOG_OBJECT);
	AddCommand(objects, ID_SCATTER_OBJECT);
	AddCommand(objects, ID_PROXY_OBJECT);

	cinema::BaseContainer menu;
	menu.InsData(cinema::MENURESOURCE_SUBTITLE, title);
	AddCommand(menu, ID_IPR_COMMAND);
	menu.InsData(cinema::MENURESOURCE_SUBMENU, materials);
	menu.InsData(cinema::MENURESOURCE_SUBMENU, objects);
	extensions->InsData(cinema::MENURESOURCE_SUBMENU, menu);
	return true;
}

} // namespace cyclesrender
