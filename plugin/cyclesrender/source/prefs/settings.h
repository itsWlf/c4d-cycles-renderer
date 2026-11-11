// SPDX-License-Identifier: Apache-2.0
// Plugin-wide settings, stored in Cinema 4D's preferences.

#ifndef CYCLESRENDER_SETTINGS_H__
#define CYCLESRENDER_SETTINGS_H__

#include "c4d.h"

namespace cyclesrender
{

/// The Blender executable chosen by the user. Empty until one is chosen or detected.
cinema::Filename GetBlenderPath();

/// The same for starting Blender: when none is chosen, a supported Blender in its usual place
/// is looked for and used for this session. May be called from any thread.
cinema::Filename GetBlenderPathOrDetect();

/// The line describing whether that executable can be used (see DescribeBlender()).
cinema::String GetBlenderStatus();

/// Stores a new executable path, runs it to learn its version, and stores the resulting status.
/// Blocks for about a second when the file exists.
void SetBlenderPath(const cinema::Filename& path);

/// Searches for a supported Blender and stores it. Returns false when none was found;
/// the stored path is then left as it was.
cinema::Bool DetectAndStoreBlender();

/// Detects Blender when the user has not chosen one yet, so that a fresh installation renders
/// without a visit to the Preferences. Does nothing once a path is stored (a path the user
/// set is never replaced) or named by g_cyclesRenderBlender. Call on the main thread; it may
/// block for about a second the one time it finds a Blender.
void DetectBlenderOnFirstStart();

} // namespace cyclesrender

#endif // CYCLESRENDER_SETTINGS_H__
