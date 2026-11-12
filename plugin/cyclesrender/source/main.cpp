// SPDX-License-Identifier: Apache-2.0
// Module entry points of the Cycles Render plugin.
//
// Cinema 4D calls these while it loads and unloads the module. Each plugin hook the module
// provides is registered from PluginStart().

#include "c4d.h"
#include "c4d_plugin.h"
#include "c4d_resource.h"

#include "ids.h"
#include "materials/baked_files.h"
#include "ipr/ipr_controller.h"
#include "ipr/ipr_dialog.h"
#include "ipr/ipr_testhook.h"
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
#include "version.h"
#include "videopost/cycles_videopost.h"

// Registers every hook of the plugin. Only the renderer is essential: without it the module
// reports failure and Cinema 4D unloads it. Any other hook that cannot be registered is named
// in the console and the rest of the plugin keeps working.
cinema::Bool cinema::PluginStart()
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
	if (!cyclesrender::RegisterCyclesMaterial())
		ApplicationOutput("Cycles Render: could not register the Cycles Material command (id @).", cyclesrender::ID_CYCLES_MATERIAL);
	if (!cyclesrender::RegisterNativeMaterial())
		ApplicationOutput("Cycles Render: could not register the Cycles Native Material (ids @ and @).", cyclesrender::ID_NATIVE_MATERIAL,
			cyclesrender::ID_NATIVE_MATERIAL_EDITS);
	if (!cyclesrender::RegisterIprCommand())
		ApplicationOutput("Cycles Render: could not register the IPR command.");
	if (!cyclesrender::RegisterBakedFilesWatch())
		ApplicationOutput("Cycles Render: could not register the watch over closed documents; baked textures stay until Cinema 4D quits.");
	ApplicationOutput("Cycles Render @: module loaded (renderer id @).", maxon::String(CYCLESRENDER_VERSION), cyclesrender::ID_CYCLES_VIDEOPOST);
	return true;
}

// Ends everything that runs on its own: windows, threads and the Blender processes the plugin
// started. The order matters: the window and the material editor use the workers freed after them.
void cinema::PluginEnd()
{
	cyclesrender::FreeIprDialog();
	cyclesrender::FreeNativeMaterialEditing();
	cyclesrender::FreeMaterialPreviews();
	// Ends the IPR's worker thread and its Blender.
	cyclesrender::IprController::Free();
	// Last, once no Blender the plugin started runs any more.
	cyclesrender::FreeBakedFiles();
	cyclesrender::FreeVolumeFiles();
}

cinema::Bool cinema::PluginMessage(cinema::Int32 id, void* data)
{
	switch (id)
	{
		case C4DPL_INIT_SYS:
		{
			// The resource system must be up before any plugin of this module can register.
			return cinema::g_resource.Init();
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
