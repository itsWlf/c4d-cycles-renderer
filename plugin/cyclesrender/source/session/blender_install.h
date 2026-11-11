// SPDX-License-Identifier: Apache-2.0
// Finding and validating the Blender executable the plugin drives, and finding the bridge
// that is installed in it.

#ifndef CYCLESRENDER_BLENDER_INSTALL_H__
#define CYCLESRENDER_BLENDER_INSTALL_H__

#include "c4d.h"
#include "maxon/url.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// What is known about one Blender executable.
//----------------------------------------------------------------------------------------
struct BlenderInstall
{
	maxon::Url executable;					///< Path of blender.exe.
	maxon::String versionText;			///< As printed by Blender, e.g. "5.2.3 LTS". Empty if it could not be run.
	maxon::Int32 major = 0;
	maxon::Int32 minor = 0;
	maxon::Int32 patch = 0;
	maxon::Bool found = false;			///< The file exists.
	maxon::Bool supported = false;	///< The version is one the bridge is validated against.
};

/// True for the Blender versions the bridge is validated against (see docs/reference/blender.md).
/// The bridge applies the same rule when it starts (SUPPORTED_BLENDER in bridge/c4d_bridge).
maxon::Bool IsSupportedBlender(maxon::Int32 major, maxon::Int32 minor);

/// Runs "<executable> --version" and reports what it is. Blocks for about a second.
/// A missing or unrunnable executable is reported in the result, not as an error.
maxon::Result<BlenderInstall> ProbeBlender(const maxon::Url& executable);

/// Searches the standard install locations and returns the newest supported Blender.
/// The result has found == false when there is none.
maxon::Result<BlenderInstall> DetectBlender();

/// One line for the user that says whether this install can be used, and why not if it cannot.
maxon::String DescribeBlender(const BlenderInstall& install);

//----------------------------------------------------------------------------------------
/// Finds the entry script of the bridge (c4d_bridge/main.py) for the Blender at @p executable.
///
/// The bridge is a Blender extension with the id "cycles_render_bridge"; the user installs
/// its .zip in Blender (Preferences > Get Extensions > Install from Disk), which unpacks it
/// into Blender's folder for user extensions. Blender is started with that script, so the
/// extension need not even be enabled. The places are tried in the order Blender itself
/// resolves its user folder:
///   1. the folder named by the environment variable BLENDER_USER_EXTENSIONS,
///   2. "extensions" inside the folder named by BLENDER_USER_RESOURCES,
///   3. "portable/extensions" next to the executable (a portable Blender),
///   4. the user's own folder, e.g. %APPDATA%\Blender Foundation\Blender\5.2\extensions,
/// and at last a copy shipped inside this plugin's own folder (res/bridge), for a setup that
/// keeps everything in one place.
///
/// @return  The script's path, or an empty Url when the bridge is installed nowhere.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Url> FindBridgeScript(const maxon::Url& executable);

} // namespace cyclesrender

#endif // CYCLESRENDER_BLENDER_INSTALL_H__
