// SPDX-License-Identifier: Apache-2.0
// Module entry points of the Cycles Render plugin.
//
// Cinema 4D calls these while it loads and unloads the module. Each plugin hook the module
// provides is registered from PluginStart().
//
// The two that return a value are written with a trailing return type: with the 2024 SDK,
// where cinema:: reads as :: (sdk_compat.h), "cinema::Bool cinema::PluginStart()" would read as
// the one name ::Bool::PluginStart.

#include "c4d.h"
#include "c4d_plugin.h"
#include "c4d_resource.h"

#include "ids.h"
#include "materials/baked_files.h"
#include "ipr/ipr_add.h"
#include "ipr/ipr_controller.h"
#include "ipr/ipr_dialog.h"
#include "ipr/ipr_scenehook.h"
#include "ipr/ipr_testhook.h"
#include "lights/light_object.h"
#include "lights/light_tag.h"
#include "objects/environment_object.h"
#include "objects/fog_object.h"
#include "objects/proxy_object.h"
#include "objects/scatter_object.h"
#include "plugin_menu.h"
#include "prefs/prefs_page.h"
#include "prefs/settings.h"
#include "scene/scene_dump.h"
#include "scene/volume_export.h"
#include "session/selftest.h"
#include "tags/camera_tag.h"
#include "tags/object_tag.h"
#include "materials/cycles_material.h"
#include "materials/material_preview.h"
#include "materials/native_material.h"
#include "sdk_compat.h"
#include "version.h"
#include "videopost/cycles_videopost.h"

// Registers every hook of the plugin. Only the renderer is essential: without it the module
// reports failure and Cinema 4D unloads it. Any other hook that cannot be registered is named
// in the console and the rest of the plugin keeps working.
auto cinema::PluginStart() -> cinema::Bool
{
	if (!cyclesrender::RegisterCyclesVideoPost())
	{
		ApplicationOutput("Cycles Render: could not register the renderer (id @).", cyclesrender::ID_CYCLES_VIDEOPOST);
		return false;
	}
	if (!cyclesrender::RegisterPreferencesPage())
		ApplicationOutput("Cycles Render: could not register the Preferences page (id @).", cyclesrender::ID_PREFERENCES);
	if (!cyclesrender::RegisterObjectTag())
		ApplicationOutput("Cycles Render: could not register the Cycles Object tag (id @).", cyclesrender::ID_OBJECT_TAG);
	if (!cyclesrender::RegisterCameraTag())
		ApplicationOutput("Cycles Render: could not register the Cycles Camera tag (id @).", cyclesrender::ID_CAMERA_TAG);
	if (!cyclesrender::RegisterLightObject())
		ApplicationOutput("Cycles Render: could not register the Cycles Light object (id @).", cyclesrender::ID_LIGHT_OBJECT);
	if (!cyclesrender::RegisterLightTag())
		ApplicationOutput("Cycles Render: could not register the Cycles Light tag (id @).", cyclesrender::ID_LIGHT_TAG);
	if (!cyclesrender::RegisterFogObject())
		ApplicationOutput("Cycles Render: could not register the Cycles Fog object (id @).", cyclesrender::ID_FOG_OBJECT);
	if (!cyclesrender::RegisterEnvironmentObject())
		ApplicationOutput("Cycles Render: could not register the Cycles Environment object (id @).", cyclesrender::ID_ENVIRONMENT_OBJECT);
	if (!cyclesrender::RegisterScatterObject())
		ApplicationOutput("Cycles Render: could not register the Cycles Scatter object (id @).", cyclesrender::ID_SCATTER_OBJECT);
	if (!cyclesrender::RegisterProxyObject())
		ApplicationOutput("Cycles Render: could not register the Cycles Proxy object (id @).", cyclesrender::ID_PROXY_OBJECT);
	if (!cyclesrender::RegisterCyclesMaterial())
		ApplicationOutput("Cycles Render: could not register the Cycles Material command (id @).", cyclesrender::ID_CYCLES_MATERIAL);
	if (!cyclesrender::RegisterCyclesWorldMaterial())
		ApplicationOutput("Cycles Render: could not register the Cycles World Material command (id @).", cyclesrender::ID_CYCLES_WORLD_MATERIAL);
	if (!cyclesrender::RegisterNativeMaterial())
		ApplicationOutput("Cycles Render: could not register the Cycles Native Material (ids @ and @).", cyclesrender::ID_NATIVE_MATERIAL,
			cyclesrender::ID_NATIVE_MATERIAL_EDITS);
	if (!cyclesrender::RegisterIprCommand())
		ApplicationOutput("Cycles Render: could not register the IPR command.");
	if (!cyclesrender::RegisterCameraCommand())
		ApplicationOutput("Cycles Render: could not register the Cycles Camera command (id @).", cyclesrender::ID_CAMERA_COMMAND);
	// Without the hook no evaluation inside a modal loop is followed (ipr/ipr_scenehook.h): the meshes of
	// parametric objects and deformers arrive when the drag has ended, lights and the camera when a frame
	// happens to arrive during it (the frame thread's call still sends those) or at its end.
	if (!cyclesrender::RegisterIprSceneHook())
		ApplicationOutput("Cycles Render: could not register the IPR's scene hook (id @); values dragged in the Attribute Manager are not followed step by step in the IPR and may arrive only when the drag ends.", cyclesrender::ID_IPR_SCENE_HOOK);
	if (!cyclesrender::RegisterBakedFilesWatch())
		ApplicationOutput("Cycles Render: could not register the watch over closed documents; baked textures stay until Cinema 4D quits.");
	// Names the Cinema 4D version this build is for: several builds can sit side by side in one
	// plugin folder, and Cinema 4D loads the one that matches it (docs/reference/c4d-sdk.md).
	ApplicationOutput("Cycles Render @ for Cinema 4D @: module loaded (renderer id @).", maxon::String(CYCLESRENDER_VERSION), CYCLESRENDER_C4D_YEAR,
		cyclesrender::ID_CYCLES_VIDEOPOST);
	return true;
}

// Ends everything that runs on its own: windows, threads and the Blender processes the plugin
// started. The order matters: the window and the material editor use the workers freed after them.
// For the IPR's scene hook, which may run on a thread that evaluates a document at this very moment:
// with the window gone it finds no modal loop to follow (IsIprInModalLoop), so no new call enters the
// controller; one that entered before is waited for by IprController::Free(), which deletes last.
void cinema::PluginEnd()
{
	cyclesrender::FreeIprDialog();
	cyclesrender::FreeAddedCameraSwitches();
	cyclesrender::FreeNativeMaterialEditing();
	cyclesrender::FreeMaterialPreviews();
	// Ends the IPR's worker thread, its frame thread and its Blender.
	cyclesrender::IprController::Free();
	// Last, once no Blender the plugin started runs any more.
	cyclesrender::FreeBakedFiles();
	cyclesrender::FreeVolumeFiles();
}

auto cinema::PluginMessage(cinema::Int32 id, void* data) -> cinema::Bool
{
	switch (id)
	{
		case C4DPL_INIT_SYS:
		{
			// The resource system must be up before any plugin of this module can register.
			return cinema::g_resource.Init();
		}
		case C4DPL_BUILDMENU:
		{
			// The plugin's commands and objects are hidden from the list Cinema 4D makes by itself; they
			// are listed in a submenu of the plugin's own, in the order the maintainer chose (plugin_menu.h).
			cyclesrender::AddPluginMenu();
			return true;
		}
		case C4DPL_STARTACTIVITY:
		{
			// All plugins are registered and the preferences are loaded: a first start without a
			// Blender chosen looks for one, so that rendering works without any setting up.
			cyclesrender::DetectBlenderOnFirstStart();
			// Baked textures a crashed session left behind; the preferences folder is known now.
			cyclesrender::RemoveBakedFilesOfEarlierSessions();
			return true;
		}
		case cyclesrender::ID_CYCLES_VIDEOPOST:
		{
			// Development hook: tests send this message. With g_cyclesRenderIprTest set it runs an IPR
			// command; with g_cyclesRenderDumpFile set it lists what the exporter finds in the active
			// document; otherwise it runs the session self-test.
			if (cyclesrender::RunIprTestCommand())
				return true;
			if (cyclesrender::IsSceneDumpRequested())
				return cyclesrender::DumpActiveDocument();
			return cyclesrender::RunSessionSelfTest();
		}
	}
	return false;
}
