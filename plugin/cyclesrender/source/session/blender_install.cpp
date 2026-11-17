// SPDX-License-Identifier: Apache-2.0
// Finding Blender and the bridge on this machine (blender_install.h).

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/file_utilities.h"
#include "maxon/iobrowse.h"
#include "maxon/system_process.h"

#include "../platform/environment.h"
#include "blender_install.h"

// Tests make detection look elsewhere, so that a machine with Blender installed can be a machine
// without one (an empty folder: detection finds nothing), without touching the installation:
//   g_cyclesRenderBlenderSearch=<folder>
MAXON_CONFIGURATION_STRING(g_cyclesRenderBlenderSearch, "", maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: the folder detection looks for Blender in, instead of C:\\Program Files\\Blender Foundation.");

namespace cyclesrender
{

// The Blender versions the bridge is validated against, oldest first. `folder` is the name
// Blender gives its per-version folders ("<major>.<minor>"), both inside an installation and
// in the user's profile. Keep this list in step with SUPPORTED_BLENDER in
// bridge/c4d_bridge/__init__.py and VERSIONS in tools/make_nodes.py (which checks it).
struct SupportedVersion
{
	maxon::Int32 major;
	maxon::Int32 minor;
	const maxon::Char* folder;
};

static const SupportedVersion SUPPORTED_VERSIONS[] =
{
	{ 5, 0, "5.0" },
	{ 5, 1, "5.1" },
	{ 5, 2, "5.2" },
	{ 5, 3, "5.3" },
};

maxon::String BlenderVersionName(maxon::Int32 code)
{
	return FormatString("@.@", code / 100, code % 100);
}

// The version detection prefers when the bridge is installed in none of the Blenders found (or
// in several): 5.2, the long-term support release, because 5.3 has open bugs (the maintainer's
// decision of 2026-11-16; docs/roadmap.md, "Open bugs"). Change it here; DetectBlender() uses it.
static const maxon::Int32 PREFERRED_MAJOR = 5;
static const maxon::Int32 PREFERRED_MINOR = 2;

maxon::Int32 OldestSupportedBlenderVersion()
{
	return BlenderVersionCode(SUPPORTED_VERSIONS[0].major, SUPPORTED_VERSIONS[0].minor);
}

maxon::String SupportedBlenderVersionsText()
{
	const maxon::Int count = SIZEOF(SUPPORTED_VERSIONS) / SIZEOF(SUPPORTED_VERSIONS[0]);
	return FormatString("@ to @", maxon::String(SUPPORTED_VERSIONS[0].folder), maxon::String(SUPPORTED_VERSIONS[count - 1].folder));
}

// Blender prints "Blender <major>.<minor>.<patch>[ <suffix>]" as the first line of --version.
static void ParseVersion(const maxon::BaseArray<maxon::Char>& output, BlenderInstall& install)
{
	static const maxon::Char PREFIX[] = "Blender ";
	const maxon::Int prefixLength = maxon::Int(sizeof(PREFIX)) - 1;
	const maxon::Int count = output.GetCount();

	maxon::Int start = NOTOK;
	for (maxon::Int i = 0; i + prefixLength <= count && start == NOTOK; ++i)
	{
		maxon::Bool match = true;
		for (maxon::Int k = 0; k < prefixLength && match; ++k)
			match = output[i + k] == PREFIX[k];
		if (match)
			start = i + prefixLength;
	}
	if (start == NOTOK)
		return;

	maxon::Int end = start;
	while (end < count && output[end] != '\n' && output[end] != '\r')
		++end;
	install.versionText = maxon::String(&output[start], end - start);

	maxon::Int32 numbers[3] = { 0, 0, 0 };
	maxon::Int index = 0;
	for (maxon::Int i = start; i < end && index < 3; ++i)
	{
		const maxon::Char c = output[i];
		if (c >= '0' && c <= '9')
			numbers[index] = numbers[index] * 10 + maxon::Int32(c - '0');
		else if (c == '.')
			++index;
		else
			break;
	}
	install.major = numbers[0];
	install.minor = numbers[1];
	install.patch = numbers[2];
	install.supported = IsSupportedBlender(install.major, install.minor);
}

// Called by the SDK once per second while Blender runs; gives up on a Blender that hangs.
static maxon::Int32 ProbeCallback(maxon::SystemProcessObjectRef process, maxon::EXECUTEPROGRAMCALLBACK command, void* userData, const maxon::Url& logFile)
{
	if (command == maxon::EXECUTEPROGRAMCALLBACK::RUNNING)
	{
		maxon::Int32* const seconds = static_cast<maxon::Int32*>(userData);
		if (++(*seconds) > 30)
			return 'stop';
	}
	return 0;
}

maxon::Bool IsSupportedBlender(maxon::Int32 major, maxon::Int32 minor)
{
	for (const SupportedVersion& version : SUPPORTED_VERSIONS)
	{
		if (version.major == major && version.minor == minor)
			return true;
	}
	return false;
}

// The version when Blender's own answer is missing. Measured in c4dpy (2026-11-16, Blender 5.0.0):
// the file was found and ran, yet the captured output of --version held no version line, so the
// IPR's first export did not know the version and sent a setting 5.0 lacks. Blender keeps its data
// in a folder named "<major>.<minor>" next to the executable, which tells the version that matters
// here (the patch stays 0). Any such folder counts, not only a supported version's: measured with
// Blender 4.2.11 (2026-11-17, B7), whose answer was missing on one start of two, so that it passed
// as "version unknown" and was started, instead of being named as not supported.
static maxon::Result<void> VersionFromDataFolder(BlenderInstall& install)
{
	iferr_scope;
	if (install.versionText.IsPopulated())
		return maxon::OK;
	const maxon::Url installFolder = install.executable.GetDirectory();
	if (installFolder.IoDetect() != maxon::IODETECT::DIRECTORY)
		return maxon::OK;
	for (const auto& it : installFolder.GetBrowseIterator(maxon::GETBROWSEITERATORFLAGS::NONE))
	{
		const maxon::IoBrowseRef& entry = (it) iferr_return;
		if (!(entry.IoGetFlags() & maxon::IOBROWSEFLAGS::IS_DIRECTORY))
			continue;
		// "<digits>.<digits>" and nothing else.
		const maxon::String name = entry.GetCurrentPath().GetName();
		maxon::Int32 numbers[2] = { 0, 0 };
		maxon::Int digits[2] = { 0, 0 };
		maxon::Int part = 0;
		maxon::Bool valid = name.GetLength() >= 3;
		for (maxon::Int i = 0; i < name.GetLength() && valid; ++i)
		{
			const maxon::Utf32Char c = name[i];
			if (c >= '0' && c <= '9' && digits[part] < 4)
			{
				numbers[part] = numbers[part] * 10 + maxon::Int32(c - '0');
				++digits[part];
			}
			else if (c == '.' && part == 0)
			{
				part = 1;
			}
			else
			{
				valid = false;
			}
		}
		if (!valid || part != 1 || digits[0] == 0 || digits[1] == 0)
			continue;
		install.major = numbers[0];
		install.minor = numbers[1];
		install.patch = 0;
		install.versionText = name;
		install.supported = IsSupportedBlender(install.major, install.minor);
		if (install.supported)
			break;	// of several (not seen), a supported one
	}
	return maxon::OK;
}

maxon::Result<BlenderInstall> ProbeBlender(const maxon::Url& executable)
{
	iferr_scope;

	BlenderInstall install;
	install.executable = executable;
	install.found = executable.IoDetect() == maxon::IODETECT::FILE;
	if (!install.found)
		return install;

	maxon::BaseArray<maxon::String> arguments;
	arguments.Append("--version"_s) iferr_return;

	maxon::Int32 seconds = 0;
	const maxon::EXECUTEPROGRAMFLAGS flags = maxon::EXECUTEPROGRAMFLAGS::CAPTURELOGFILE |
		maxon::EXECUTEPROGRAMFLAGS::DONT_DELETE_LOGFILE | maxon::EXECUTEPROGRAMFLAGS::HIDE_APPLICATION;

	iferr (maxon::SystemProcessObjectRef process = maxon::SystemProcessInterface::StartProcessAndWait(executable, arguments, flags, ProbeCallback, &seconds))
	{
		VersionFromDataFolder(install) iferr_return;
		return install; // Not something that can be run, or no version folder: "found but no version".
	}

	const maxon::Url logFile = process.GetLogFile();
	maxon::BaseArray<maxon::Char> output;
	ifnoerr (maxon::FileUtilities::ReadFileToMemory(maxon::Url(logFile), output))
	{
		ParseVersion(output, install);
	}
	logFile.IoDelete(true) iferr_ignore("Only a temporary log file.");
	VersionFromDataFolder(install) iferr_return;
	return install;
}

// Where the bridge lies when it was installed into this Blender itself: the extensions folder
// of this version's own profile (%APPDATA%\Blender Foundation\Blender\<major>.<minor>), or of a
// portable installation. FindBridgeScript() also takes a bridge from another version's profile;
// this does not, so that detection can prefer the Blender the user installed the bridge into.
static maxon::Result<maxon::Bool> HasOwnBridge(const BlenderInstall& install);

maxon::Result<BlenderInstall> DetectBlender()
{
	iferr_scope;

	BlenderInstall best;
	maxon::Bool bestHasBridge = false;
	maxon::Bool bestPreferred = false;

#ifdef MAXON_TARGET_WINDOWS
	// The official installer puts each version in its own folder below this one (tests name another).
	maxon::Url root("file:///C:/Program Files/Blender Foundation"_s);
	if (!g_cyclesRenderBlenderSearch.IsEmpty())
	{
		root.SetSystemPath(maxon::String(g_cyclesRenderBlenderSearch)) iferr_return;
	}
	if (root.IoDetect() != maxon::IODETECT::DIRECTORY)
		return best;

	for (const auto& it : root.GetBrowseIterator(maxon::GETBROWSEITERATORFLAGS::NONE))
	{
		const maxon::IoBrowseRef& entry = (it) iferr_return;
		if (!(entry.IoGetFlags() & maxon::IOBROWSEFLAGS::IS_DIRECTORY))
			continue;

		const maxon::Url executable = (entry.GetCurrentPath() + "blender.exe"_s) iferr_return;
		if (executable.IoDetect() != maxon::IODETECT::FILE)
			continue;

		// Running every install would take seconds, so only run the ones that carry a data
		// folder named after a supported version, e.g. "Blender 5.2/5.2".
		maxon::Bool candidate = false;
		for (const SupportedVersion& version : SUPPORTED_VERSIONS)
		{
			const maxon::Url dataFolder = (entry.GetCurrentPath() + maxon::String(version.folder)) iferr_return;
			candidate = candidate || dataFolder.IoDetect() == maxon::IODETECT::DIRECTORY;
		}
		if (!candidate)
			continue;

		const BlenderInstall install = ProbeBlender(executable) iferr_return;
		if (!install.supported)
			continue;
		// Of several supported versions: the one the bridge was installed into (a user installs it
		// into one Blender; another version would not find it in its own profile); then the
		// preferred version (PREFERRED_MAJOR, PREFERRED_MINOR); then the newest.
		const maxon::Bool hasBridge = HasOwnBridge(install) iferr_return;
		const maxon::Bool preferred = install.major == PREFERRED_MAJOR && install.minor == PREFERRED_MINOR;
		const maxon::Bool newer = install.major != best.major ? install.major > best.major :
			install.minor != best.minor ? install.minor > best.minor : install.patch > best.patch;
		const maxon::Bool better = hasBridge != bestHasBridge ? hasBridge : preferred != bestPreferred ? preferred : newer;
		if (!best.found || better)
		{
			best = install;
			bestHasBridge = hasBridge;
			bestPreferred = preferred;
		}
	}
#endif

	return best;
}

// Where the bridge's entry script lies below an extensions folder of Blender. "user_default"
// is the repository "Install from Disk" installs into; "cycles_render_bridge" is the id of the
// extension (bridge/blender_manifest.toml).
static maxon::Result<maxon::Url> BridgeScriptIn(const maxon::Url& extensionsFolder)
{
	iferr_scope;

	maxon::Url script = extensionsFolder;
	script.Append("user_default"_s) iferr_return;
	script.Append("cycles_render_bridge"_s) iferr_return;
	script.Append("c4d_bridge"_s) iferr_return;
	script.Append("main.py"_s) iferr_return;
	return script;
}

// The folder an environment variable names, or an empty Url when the variable is not set.
static maxon::Url EnvironmentFolder(const maxon::Char* name)
{
	maxon::Char text[4096];
	if (!platform::GetEnvironmentText(name, text, int(sizeof(text))))
		return maxon::Url();
	maxon::Url folder;
	iferr (folder.SetSystemPath(maxon::String(text)))
		return maxon::Url();
	return folder;
}

maxon::Result<maxon::Url> FindBridgeScript(const maxon::Url& executable)
{
	iferr_scope;

	// The folders that may hold Blender's user extensions, most specific first.
	maxon::BaseArray<maxon::Url> extensionFolders;

	const maxon::Url fromEnvironment = EnvironmentFolder("BLENDER_USER_EXTENSIONS");
	if (fromEnvironment.IsPopulated())
	{
		extensionFolders.Append(fromEnvironment) iferr_return;
	}

	const maxon::Url resources = EnvironmentFolder("BLENDER_USER_RESOURCES");
	if (resources.IsPopulated())
	{
		const maxon::Url folder = (resources + "extensions"_s) iferr_return;
		extensionFolders.Append(folder) iferr_return;
	}

	const maxon::Url installFolder = executable.GetDirectory();
	if (installFolder.IsPopulated())
	{
		maxon::Url folder = installFolder;
		folder.Append("portable"_s) iferr_return;
		folder.Append("extensions"_s) iferr_return;
		extensionFolders.Append(folder) iferr_return;
	}

#ifdef MAXON_TARGET_WINDOWS
	// Blender keeps one profile per version. The version of this executable is the one whose
	// data folder lies next to it ("Blender 5.2/5.2"); its profile is tried first.
	const maxon::Url appData = EnvironmentFolder("APPDATA");
	if (appData.IsPopulated())
	{
		for (const maxon::Bool ofThisInstall : { true, false })
		{
			for (const SupportedVersion& version : SUPPORTED_VERSIONS)
			{
				maxon::Bool hasDataFolder = false;
				if (installFolder.IsPopulated())
				{
					const maxon::Url dataFolder = (installFolder + maxon::String(version.folder)) iferr_return;
					hasDataFolder = dataFolder.IoDetect() == maxon::IODETECT::DIRECTORY;
				}
				if (hasDataFolder != ofThisInstall)
					continue;
				maxon::Url folder = appData;
				folder.Append("Blender Foundation"_s) iferr_return;
				folder.Append("Blender"_s) iferr_return;
				folder.Append(maxon::String(version.folder)) iferr_return;
				folder.Append("extensions"_s) iferr_return;
				extensionFolders.Append(folder) iferr_return;
			}
		}
	}
#endif

	for (const maxon::Url& folder : extensionFolders)
	{
		const maxon::Url script = BridgeScriptIn(folder) iferr_return;
		if (script.IoDetect() == maxon::IODETECT::FILE)
			return script;
	}

	// A copy inside the plugin's own folder.
	maxon::Url bundled = cinema::MaxonConvert(cinema::GeGetPluginPath(), cinema::MAXONCONVERTMODE::NONE);
	bundled.Append("res"_s) iferr_return;
	bundled.Append("bridge"_s) iferr_return;
	bundled.Append("c4d_bridge"_s) iferr_return;
	bundled.Append("main.py"_s) iferr_return;
	if (bundled.IoDetect() == maxon::IODETECT::FILE)
		return bundled;

	return maxon::Url();
}

static maxon::Result<maxon::Bool> HasOwnBridge(const BlenderInstall& install)
{
	iferr_scope;
	const maxon::Url installFolder = install.executable.GetDirectory();
	maxon::BaseArray<maxon::Url> extensionFolders;
	if (installFolder.IsPopulated())
	{
		maxon::Url folder = installFolder;
		folder.Append("portable"_s) iferr_return;
		folder.Append("extensions"_s) iferr_return;
		extensionFolders.Append(folder) iferr_return;
	}
#ifdef MAXON_TARGET_WINDOWS
	const maxon::Url appData = EnvironmentFolder("APPDATA");
	if (appData.IsPopulated())
	{
		maxon::Url folder = appData;
		folder.Append("Blender Foundation"_s) iferr_return;
		folder.Append("Blender"_s) iferr_return;
		folder.Append(FormatString("@.@", install.major, install.minor)) iferr_return;
		folder.Append("extensions"_s) iferr_return;
		extensionFolders.Append(folder) iferr_return;
	}
#endif
	for (const maxon::Url& folder : extensionFolders)
	{
		const maxon::Url script = BridgeScriptIn(folder) iferr_return;
		if (script.IoDetect() == maxon::IODETECT::FILE)
			return true;
	}
	return false;
}

maxon::String DescribeBlender(const BlenderInstall& install)
{
	if (install.executable.IsEmpty())
		return "No Blender selected. Choose blender.exe or press Detect."_s;
	if (!install.found)
		return "No file exists at this path."_s;
	if (install.versionText.IsEmpty())
		return "This file did not answer as Blender."_s;
	if (!install.supported)
		return FormatString("Blender @ is not supported. Cycles Render needs Blender @.", install.versionText, SupportedBlenderVersionsText());
	return FormatString("Blender @ - ready.", install.versionText);
}

} // namespace cyclesrender
