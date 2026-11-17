// SPDX-License-Identifier: Apache-2.0
// The plugin's own settings in Cinema 4D's preferences: which Blender to use, and the switches of
// the Preferences page (settings.h).

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/spinlock.h"

#include "vpcyclesrender.h"

#include "../ids.h"
#include "../session/blender_install.h"
#include "settings.h"

// Tests and render nodes can name the Blender to use without touching the Preferences:
//   g_cyclesRenderBlender=<path to blender.exe>
MAXON_CONFIGURATION_STRING(g_cyclesRenderBlender, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: path of the Blender executable. Empty: use the one set in the Preferences.");

namespace cyclesrender
{

// Keys inside the plugin's container in Cinema 4D's preferences. Never renumber, never reuse.
// (prefs/devices.cpp uses 1010 to 1012, the IPR's test hook 9000 and 9001. 1006 to 1009 and 1013
// to 1019 have never been used; 1022 on is kept for further switches of the IPR, 1031 on for
// further defaults of new render settings.)
enum
{
	SETTING_BLENDER_PATH = 1000,
	SETTING_BLENDER_STATUS = 1001,
	SETTING_BLENDER_MAJOR = 1002,		///< the version of the Blender at SETTING_BLENDER_PATH, learned when it was stored
	SETTING_BLENDER_MINOR = 1003,
	SETTING_BLENDER_PATCH = 1004,
	SETTING_BLENDER_VERSION_TEXT = 1005,
	SETTING_IPR_SKIP_UNCHANGED = 1020,
	SETTING_IPR_NAVIGATE = 1021,
	SETTING_DEFAULT_ENGINE = 1030		///< 0.2.0 (F21): GetDefaultEngine()
};

// The container is keyed by the renderer's issued plugin ID so it survives ID changes elsewhere.
static cinema::BaseContainer ReadSettings()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	return stored ? *stored : cinema::BaseContainer();
}

// The Blender in use and its version, by path: GetBlenderInUse().
static maxon::Spinlock g_inUseLock;
// cinema::String like the Filename strings it is compared with: with the 2024 SDK (C++17) a
// maxon::String compared with a cinema::String is ambiguous.
static cinema::String g_inUsePath;
static BlenderInstall g_inUse;
static maxon::Bool g_inUseKnown = false;

static void RememberBlender(const cinema::Filename& path, const BlenderInstall& install)
{
	maxon::ScopedLock guard(g_inUseLock);
	g_inUsePath = path.GetString();
	g_inUse = install;
	g_inUseKnown = true;
}

static void StoreBlender(const BlenderInstall& install, const cinema::Filename& path)
{
	cinema::BaseContainer settings = ReadSettings();
	settings.SetFilename(SETTING_BLENDER_PATH, path);
	settings.SetString(SETTING_BLENDER_STATUS, DescribeBlender(install));
	settings.SetInt32(SETTING_BLENDER_MAJOR, install.major);
	settings.SetInt32(SETTING_BLENDER_MINOR, install.minor);
	settings.SetInt32(SETTING_BLENDER_PATCH, install.patch);
	settings.SetString(SETTING_BLENDER_VERSION_TEXT, install.versionText);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, settings, false);
	if (install.found)
		RememberBlender(path, install);
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

BlenderInstall GetBlenderInUse()
{
	const cinema::Filename path = GetBlenderPathOrDetect();
	BlenderInstall install;
	if (!path.IsPopulated())
		return install;
	{
		maxon::ScopedLock guard(g_inUseLock);
		if (g_inUseKnown && g_inUsePath == path.GetString())
			return g_inUse;
	}

	// The version stored with the path in the preferences (since 0.2.0), else ask the executable.
	const cinema::BaseContainer settings = ReadSettings();
	install.executable = cinema::MaxonConvert(path, cinema::MAXONCONVERTMODE::NONE);
	if (g_cyclesRenderBlender.IsEmpty() && settings.GetFilename(SETTING_BLENDER_PATH) == path && settings.GetInt32(SETTING_BLENDER_MAJOR) > 0)
	{
		install.found = true;
		install.major = settings.GetInt32(SETTING_BLENDER_MAJOR);
		install.minor = settings.GetInt32(SETTING_BLENDER_MINOR);
		install.patch = settings.GetInt32(SETTING_BLENDER_PATCH);
		install.versionText = settings.GetString(SETTING_BLENDER_VERSION_TEXT);
		install.supported = IsSupportedBlender(install.major, install.minor);
	}
	else
	{
		maxon::Result<BlenderInstall> probed = ProbeBlender(install.executable);
		if (probed == maxon::OK)
			install = probed.GetValue();
	}
	// A path without a file is not remembered: the file may be put there later.
	if (install.found)
		RememberBlender(path, install);
	return install;
}

void NoteBlenderVersion(const cinema::Filename& executable, maxon::Int32 major, maxon::Int32 minor, maxon::Int32 patch,
	const maxon::String& versionText)
{
	maxon::ScopedLock guard(g_inUseLock);
	if (!g_inUseKnown || g_inUsePath != executable.GetString())
		return;
	if (g_inUse.major == major && g_inUse.minor == minor && g_inUse.patch == patch)
		return;
	g_inUse.found = true;
	g_inUse.major = major;
	g_inUse.minor = minor;
	g_inUse.patch = patch;
	g_inUse.versionText = versionText;
	g_inUse.supported = IsSupportedBlender(major, minor);
}

maxon::String BlenderPreferencesPlace()
{
	return "Preferences > Renderer > Cycles Render"_s;
}

maxon::String GetBlenderProblem()
{
	const cinema::Filename path = GetBlenderPathOrDetect();
	if (!path.IsPopulated())
	{
		return FormatString("No Blender is selected, and no Blender @ was found where its installer puts it. Choose blender.exe in @.",
			SupportedBlenderVersionsText(), BlenderPreferencesPlace());
	}
	// A path named at start (tests, render nodes) is not the user's choice in the Preferences.
	const maxon::Bool named = !g_cyclesRenderBlender.IsEmpty();
	const maxon::Url executable = cinema::MaxonConvert(path, cinema::MAXONCONVERTMODE::NONE);
	if (executable.IoDetect() != maxon::IODETECT::FILE)
	{
		if (named)
			return FormatString("The Blender named by g_cyclesRenderBlender does not exist: @", path.GetString());
		return FormatString("The Blender selected in @ does not exist: @. Choose blender.exe there again.", BlenderPreferencesPlace(), path.GetString());
	}
	const BlenderInstall install = GetBlenderInUse();
	if (install.found && install.Code() > 0 && !install.supported)
	{
		return FormatString("Blender @ is not supported. Cycles Render needs Blender @: choose one in @.", install.versionText,
			SupportedBlenderVersionsText(), named ? "g_cyclesRenderBlender"_s : BlenderPreferencesPlace());
	}
	return maxon::String();
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

cinema::Bool GetIprNavigate()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	return stored ? stored->GetBool(SETTING_IPR_NAVIGATE, true) : true;
}

void SetIprNavigate(cinema::Bool navigate)
{
	cinema::BaseContainer settings = ReadSettings();
	settings.SetBool(SETTING_IPR_NAVIGATE, navigate);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, settings, false);
}

maxon::Int32 GetDefaultEngine()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	const maxon::Int32 engine = stored ? stored->GetInt32(SETTING_DEFAULT_ENGINE, CYCLES_ENGINE_CYCLES) : CYCLES_ENGINE_CYCLES;
	return engine == CYCLES_ENGINE_EEVEE ? CYCLES_ENGINE_EEVEE : CYCLES_ENGINE_CYCLES;
}

void SetDefaultEngine(maxon::Int32 engine)
{
	cinema::BaseContainer settings = ReadSettings();
	settings.SetInt32(SETTING_DEFAULT_ENGINE, engine == CYCLES_ENGINE_EEVEE ? CYCLES_ENGINE_EEVEE : CYCLES_ENGINE_CYCLES);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, settings, false);
}

void DetectBlenderOnFirstStart()
{
	if (!g_cyclesRenderBlender.IsEmpty() || ReadSettings().GetFilename(SETTING_BLENDER_PATH).IsPopulated())
		return;
	if (DetectAndStoreBlender())
	{
		ApplicationOutput("Cycles Render: found @ and will use it. Another one can be chosen in @.", GetBlenderPath().GetString(), BlenderPreferencesPlace());
	}
	else
	{
		// Said at every start until one is chosen: nothing renders before.
		ApplicationOutput("Cycles Render: no Blender is selected, and no Blender @ was found where its installer puts it. Choose blender.exe in @.",
			SupportedBlenderVersionsText(), BlenderPreferencesPlace());
	}
}

} // namespace cyclesrender
