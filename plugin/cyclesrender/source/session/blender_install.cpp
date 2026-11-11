// SPDX-License-Identifier: Apache-2.0
// Finding Blender and the bridge on this machine (blender_install.h).

#include "c4d.h"
#include "maxon/file_utilities.h"
#include "maxon/iobrowse.h"
#include "maxon/system_process.h"

#include "../platform/environment.h"
#include "blender_install.h"

namespace cyclesrender
{

// The Blender versions the bridge is validated against, newest first. `folder` is the name
// Blender gives its per-version folders ("<major>.<minor>"), both inside an installation and
// in the user's profile. Keep this list in step with SUPPORTED_BLENDER in
// bridge/c4d_bridge/__init__.py, and with the message in DescribeBlender().
struct BlenderVersion
{
	maxon::Int32 major;
	maxon::Int32 minor;
	const maxon::Char* folder;
};

static const BlenderVersion SUPPORTED_VERSIONS[] =
{
	{ 5, 2, "5.2" },
};

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
	for (const BlenderVersion& version : SUPPORTED_VERSIONS)
	{
		if (version.major == major && version.minor == minor)
			return true;
	}
	return false;
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
		return install; // Not something that can be run: reported as "found but no version".

	const maxon::Url logFile = process.GetLogFile();
	maxon::BaseArray<maxon::Char> output;
	iferr (maxon::FileUtilities::ReadFileToMemory(maxon::Url(logFile), output))
		return install;
	logFile.IoDelete(true) iferr_ignore("Only a temporary log file.");

	ParseVersion(output, install);
	return install;
}

maxon::Result<BlenderInstall> DetectBlender()
{
	iferr_scope;

	BlenderInstall best;

#ifdef MAXON_TARGET_WINDOWS
	// The official installer puts each version in its own folder below this one.
	const maxon::Url root("file:///C:/Program Files/Blender Foundation"_s);
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
		for (const BlenderVersion& version : SUPPORTED_VERSIONS)
		{
			const maxon::Url dataFolder = (entry.GetCurrentPath() + maxon::String(version.folder)) iferr_return;
			candidate = candidate || dataFolder.IoDetect() == maxon::IODETECT::DIRECTORY;
		}
		if (!candidate)
			continue;

		const BlenderInstall install = ProbeBlender(executable) iferr_return;
		if (!install.supported)
			continue;
		const maxon::Bool newer = install.major != best.major ? install.major > best.major :
			install.minor != best.minor ? install.minor > best.minor : install.patch > best.patch;
		if (!best.found || newer)
			best = install;
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
			for (const BlenderVersion& version : SUPPORTED_VERSIONS)
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

maxon::String DescribeBlender(const BlenderInstall& install)
{
	if (install.executable.IsEmpty())
		return "No Blender selected. Choose blender.exe or press Detect."_s;
	if (!install.found)
		return "No file exists at this path."_s;
	if (install.versionText.IsEmpty())
		return "This file did not answer as Blender."_s;
	if (!install.supported)
		return FormatString("Blender @ is not supported. Cycles Render needs Blender 5.2.", install.versionText);
	return FormatString("Blender @ - ready.", install.versionText);
}

} // namespace cyclesrender
