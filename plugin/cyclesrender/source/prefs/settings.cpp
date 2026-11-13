// SPDX-License-Identifier: Apache-2.0
// The plugin's own settings in Cinema 4D's preferences: which Blender to use (settings.h).

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/spinlock.h"

#include "../ids.h"
#include "../session/blender_install.h"
#include "settings.h"

// Tests and render nodes can name the Blender to use without touching the Preferences:
//   g_cyclesRenderBlender=<path to blender.exe>
MAXON_CONFIGURATION_STRING(g_cyclesRenderBlender, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: path of the Blender executable. Empty: use the one set in the Preferences.");

namespace cyclesrender
{

// Keys inside the plugin's container in Cinema 4D's preferences. Never renumber. (prefs/devices.cpp
// uses 1010 to 1012, the IPR's test hook 9000 and 9001.)
enum
{
	SETTING_BLENDER_PATH = 1000,
	SETTING_BLENDER_STATUS = 1001,
	SETTING_IPR_SKIP_UNCHANGED = 1020
};

// The container is keyed by the renderer's issued plugin ID so it survives ID changes elsewhere.
static cinema::BaseContainer ReadSettings()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	return stored ? *stored : cinema::BaseContainer();
}

static void StoreBlender(const BlenderInstall& install, const cinema::Filename& path)
{
	cinema::BaseContainer settings = ReadSettings();
	settings.SetFilename(SETTING_BLENDER_PATH, path);
	settings.SetString(SETTING_BLENDER_STATUS, DescribeBlender(install));
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, settings, false);
}

cinema::Filename GetBlenderPath()
{
	if (!g_cyclesRenderBlender.IsEmpty())
		return cinema::Filename(g_cyclesRenderBlender);
	return ReadSettings().GetFilename(SETTING_BLENDER_PATH);
}

cinema::Filename GetBlenderPathOrDetect()
{
	const cinema::Filename chosen = GetBlenderPath();
	if (chosen.IsPopulated())
		return chosen;

	// Nothing is stored: the look at the first start found none, or has not happened yet. Look
	// again, once per session at most every few seconds, and use what is found for this session.
	// It is not stored from here: this runs on render threads, and the preferences are written
	// on the main thread only.
	static maxon::Spinlock lock;
	static cinema::Filename detected;
	static maxon::Float64 lookedAt = -1.0e30;
	maxon::ScopedLock guard(lock);
	const maxon::Float64 now = maxon::TimeValue::GetTime().GetSeconds();
	if (!detected.IsPopulated() && now - lookedAt > 10.0)
	{
		lookedAt = now;
		ifnoerr (const BlenderInstall install = DetectBlender())
		{
			if (install.found && install.supported)
				detected = cinema::MaxonConvert(install.executable);
		}
	}
	return detected;
}

cinema::String GetBlenderStatus()
{
	const cinema::BaseContainer settings = ReadSettings();
	if (settings.FindIndex(SETTING_BLENDER_STATUS) == NOTOK)
		return DescribeBlender(BlenderInstall());
	return settings.GetString(SETTING_BLENDER_STATUS);
}

void SetBlenderPath(const cinema::Filename& path)
{
	BlenderInstall install;
	if (path.IsPopulated())
	{
		const maxon::Url executable = cinema::MaxonConvert(path, cinema::MAXONCONVERTMODE::NONE);
		maxon::Result<BlenderInstall> probed = ProbeBlender(executable);
		if (probed == maxon::OK)
			install = probed.GetValue();
		else
			install.executable = executable;
	}
	StoreBlender(install, path);
}

cinema::Bool DetectAndStoreBlender()
{
	iferr (const BlenderInstall install = DetectBlender())
		return false;
	if (!install.found || !install.supported)
		return false;
	StoreBlender(install, cinema::MaxonConvert(install.executable));
	return true;
}

cinema::Bool GetIprSkipUnchangedMeshes()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	return stored ? stored->GetBool(SETTING_IPR_SKIP_UNCHANGED, true) : true;
}

void SetIprSkipUnchangedMeshes(cinema::Bool skip)
{
	cinema::BaseContainer settings = ReadSettings();
	settings.SetBool(SETTING_IPR_SKIP_UNCHANGED, skip);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, settings, false);
}

void DetectBlenderOnFirstStart()
{
	if (!g_cyclesRenderBlender.IsEmpty() || ReadSettings().GetFilename(SETTING_BLENDER_PATH).IsPopulated())
		return;
	if (DetectAndStoreBlender())
		ApplicationOutput("Cycles Render: found @ and will use it. Another one can be chosen in Preferences > Renderer > Cycles Render.", GetBlenderPath().GetString());
}

} // namespace cyclesrender
