// SPDX-License-Identifier: Apache-2.0
// Plugin-wide settings, stored in Cinema 4D's preferences.

#ifndef CYCLESRENDER_SETTINGS_H__
#define CYCLESRENDER_SETTINGS_H__

#include "c4d.h"

#include "../session/blender_install.h"

namespace cyclesrender
{

/// The Blender executable chosen by the user. Empty until one is chosen or detected.
cinema::Filename GetBlenderPath();

/// The same for starting Blender: when none is chosen, a supported Blender in its usual place
/// is looked for and used for this session. May be called from any thread.
cinema::Filename GetBlenderPathOrDetect();

/// The line describing whether that executable can be used (see DescribeBlender()).
cinema::String GetBlenderStatus();

/// Where the user chooses the Blender, for messages: "Preferences > Renderer > Cycles Render".
maxon::String BlenderPreferencesPlace();

//----------------------------------------------------------------------------------------
/// Why no Blender can be started now, in words for the user that say where to choose one (0.2.0,
/// B7); empty when the Blender in use (GetBlenderPathOrDetect()) can be started. Tells apart: none
/// is selected and detection found none; the selected file does not exist; a Blender whose version
/// is not supported (as DescribeBlender() says it). A file whose version could not be learned is
/// started, and the bridge decides. Every start of Blender asks this first (session/blender_process.h),
/// the IPR before it starts (ipr/ipr_controller.h). May be called from any thread; it may block for
/// about a second the first time it meets a path (GetBlenderInUse()).
//----------------------------------------------------------------------------------------
maxon::String GetBlenderProblem();

//----------------------------------------------------------------------------------------
/// The Blender that renders (GetBlenderPathOrDetect()) and its version, for what depends on the
/// version: nodes, sockets and settings a version lacks are left out of what is sent, and the
/// Node Editor marks nodes it lacks. Learned by running the executable once per path and session
/// (about a second; SetBlenderPath() and detection fill it at once), then kept in memory. May be
/// called from any thread. found is false when there is no Blender; Code() is 0 when its version
/// could not be learned.
//----------------------------------------------------------------------------------------
BlenderInstall GetBlenderInUse();

/// What a Blender started from @p executable reported itself (the bridge's hello): corrects the
/// version kept for that path, should the executable have been replaced by another version.
void NoteBlenderVersion(const cinema::Filename& executable, maxon::Int32 major, maxon::Int32 minor, maxon::Int32 patch,
	const maxon::String& versionText);

/// Stores a new executable path, runs it to learn its version, and stores the resulting status.
/// Blocks for about a second when the file exists.
void SetBlenderPath(const cinema::Filename& path);

/// Searches for a supported Blender and stores it. Returns false when none was found;
/// the stored path is then left as it was.
cinema::Bool DetectAndStoreBlender();

/// Detects Blender when the user has not chosen one yet, so that a fresh installation renders
/// without a visit to the Preferences. Does nothing once a path is stored (a path the user
/// set is never replaced) or named by g_cyclesRenderBlender. Says in the console what it found,
/// or that it found none and where to choose one. Call on the main thread; it may block for
/// about a second the one time it finds a Blender.
void DetectBlenderOnFirstStart();

/// Preferences > Renderer > Cycles Render > IPR, "Skip Unchanged Meshes": the IPR leaves a mesh
/// Blender holds as it is when nothing it is made from changed, instead of exporting it again after
/// every change (scene/scene_exporter.h, ExportSettings::reuse). On by default; off is the way the
/// IPR worked before, for hunting a fault.
cinema::Bool GetIprSkipUnchangedMeshes();
void SetIprSkipUnchangedMeshes(cinema::Bool skip);

/// Preferences > Renderer > Cycles Render > IPR, "Navigate in the IPR Window" (0.2.0, F17): a drag
/// with Cinema 4D's navigation keys in the IPR's picture moves the camera the IPR renders through
/// (ipr/ipr_navigation.h). On by default; off, such a drag pans the picture as every drag did before.
cinema::Bool GetIprNavigate();
void SetIprNavigate(cinema::Bool navigate);

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > New Render Settings, "Default Engine" (0.2.0, F21):
/// CYCLES_ENGINE_CYCLES (the default) or CYCLES_ENGINE_EEVEE (res/description/vpcyclesrender.h).
/// The engine Cycles Render's settings start with when they are made new: when the renderer is
/// added to render settings that did not have it (videopost/cycles_videopost.cpp, Init), and the
/// settings the IPR and others use for a document without them (GetDocumentRenderSettings). Settings
/// that are loaded or copied keep the engine they store. The Render Settings page's own default
/// stays Blender's (Cycles; tools/make_settings.py).
//----------------------------------------------------------------------------------------
maxon::Int32 GetDefaultEngine();
void SetDefaultEngine(maxon::Int32 engine);

} // namespace cyclesrender

#endif // CYCLESRENDER_SETTINGS_H__
