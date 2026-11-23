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
/// Why no Blender can be started now, in words for the user that say where to choose one; empty
/// when the Blender in use (GetBlenderPathOrDetect()) can be started. Tells apart: none is selected
/// and detection found none; the selected file does not exist; a Blender whose version is not
/// supported (as DescribeBlender() says it). A file whose version could not be learned is started,
/// and the bridge decides. Every start of Blender asks this first (session/blender_process.h), the
/// IPR before it starts (ipr/ipr_controller.h). May be called from any thread; it may block for
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

/// Preferences > Renderer > Cycles Render > IPR, "Navigate in the IPR Window": a drag
/// with Cinema 4D's navigation keys in the IPR's picture moves the camera the IPR renders through
/// (ipr/ipr_navigation.h). On by default; off, such a drag pans the picture as every drag did before.
cinema::Bool GetIprNavigate();
void SetIprNavigate(cinema::Bool navigate);

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > IPR, "Live Updates During Drags and Playback". On (the
/// default): the IPR follows while a change is being made: every step of a tool's drag in a view, a
/// value dragged in the Attribute Manager (the scene hook, ipr/ipr_scenehook.h), each frame while the
/// timeline plays. Off: Blender gets a change once it is finished (the
/// mouse released, the value set, playback stopped), through the change event and the window's timer;
/// in a modal loop nothing is sent and the scene hook does nothing; while the timeline plays nothing is
/// sent. Navigating in the IPR's own picture follows its drag either way ("Navigate in the IPR Window"
/// is its switch). Read at every use: a change applies to a running IPR at once. Main thread only
/// (ipr/ipr_dialog.cpp keeps a copy for other threads).
//----------------------------------------------------------------------------------------
cinema::Bool GetIprLiveUpdates();
void SetIprLiveUpdates(cinema::Bool live);

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > IPR, "Optimized Live Updates". What the IPR sends while
/// a change is being made, when "Live Updates During Drags and Playback" is on (without that nothing
/// is sent during a change, and this switch is greyed out and has no effect).
/// On (the default, and what keeps Cinema 4D's interface free in heavy scenes): only what moves is
/// sent, and the whole scene is exported once when the change has ended. A camera that moves is sent
/// alone; so is the place of an object dragged with a tool or in the Coordinates tab, with the objects
/// below it, and every light's values and place; a camera's values dragged in the Attribute Manager
/// follow too. What cannot be sent alone is exported whole in that step (the rules:
/// IprController::FollowStep).
/// Off: the whole scene is exported at every step of a tool's drag, and of an Attribute Manager drag
/// only the lights follow.
/// The timeline is exported whole at every frame either way, and so is every finished edit. Not a
/// switch: redraws that changed nothing export nothing (IprController::NoteRedraw), and a generator's
/// unchanged mesh is recognised without a pass over its data (scene/mesh_export.h, MESHHASH).
/// Read at every use: a change applies to a running IPR at once. Main thread only.
//----------------------------------------------------------------------------------------
cinema::Bool GetIprOptimizedUpdates();
void SetIprOptimizedUpdates(cinema::Bool optimized);

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > IPR, "Refresh Whole Scene Only on Reload". For scenes in
/// which the whole export after every change is what makes Cinema 4D lag. It counts only when "Live
/// Updates During Drags and Playback" and "Optimized Live Updates" are both on (else it is greyed out
/// and has no effect).
/// Off (the default): as described above, the whole scene is exported when a change has ended.
/// On: the IPR sends only what a step can send alone (the camera, the lights, the place of objects, the
/// meshes of single objects, the world of Cycles Environments), also when a change has ended and when
/// the time changes, and never exports the scene whole by itself. A change a step cannot send (a
/// material, a tag, a new or deleted object, a render dot, hair, a volume, the render settings) is not
/// sent: the IPR's status line says that the scene has changed, and the Reload button then sends what
/// differs. So the picture can be out of date until Reload is pressed; that is the switch's purpose.
/// The rules: IprController, "The manual mode". Read at every tick of the IPR: a change applies to a
/// running IPR at once. Main thread only.
//----------------------------------------------------------------------------------------
cinema::Bool GetIprManualRefresh();
void SetIprManualRefresh(cinema::Bool manual);

/// The development setting g_cyclesRenderIprUnoptimized (docs/development.md): the IPR exports
/// without the optimizations of its live updates, for measuring what they gain on a given machine
/// and scene with one build. Every redraw exports the scene (no look at the change counters, no
/// settle export), a generated mesh is hashed byte by byte, and "Optimized Live Updates" counts as off.
maxon::Bool IsIprUnoptimized();

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > IPR, "CPU Threads Left Free": how many of
/// the computer's logical processors a Cycles render on the CPU in the IPR leaves to Cinema 4D. The
/// IPR sends Blender a fixed thread count of all logical processors less this many (at least one; a
/// smaller fixed count of the Render Settings is kept), so its CPU render is that much slower and
/// Cinema 4D's interface and viewport always find processors that Blender does not use. Final frames
/// are not affected: they use the Render Settings' threads (the machine is meant for them then).
/// Changing Cycles' thread count recreates its render session, so the count goes with the render
/// settings: it is sent when the IPR starts and when the settings or this preference change, never
/// during a drag. While nothing is stored: a quarter of the logical processors, at least 2
/// (DefaultIprThreadsFree; measured, docs/reference/c4d-sdk.md, "Priority and cores of the IPR's
/// Blender": 8 of 32 here took a drag's main thread from 93 % to 24 % busy, 2 of 32 only to 84 %;
/// 2 of 8 on a stand-in of 8 processors from 71 % to 14 %). Read at every export of the settings.
//----------------------------------------------------------------------------------------
static const maxon::Int32 IPR_THREADS_FREE_MAX = 256;
maxon::Int32 GetIprThreadsFree();
/// Stores "CPU Threads Left Free", clamped to 0..IPR_THREADS_FREE_MAX.
void SetIprThreadsFree(maxon::Int32 threads);
/// The default of "CPU Threads Left Free" on this computer: a quarter of its logical processors, at
/// least 2, and never all of them.
maxon::Int32 DefaultIprThreadsFree();

//----------------------------------------------------------------------------------------
/// Preferences > Renderer > Cycles Render > New Render Settings, "Default Engine":
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
