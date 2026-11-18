// SPDX-License-Identifier: Apache-2.0
// The IPR: keeps a live render of the active document running in a windowed Blender.

#ifndef CYCLESRENDER_IPR_CONTROLLER_H__
#define CYCLESRENDER_IPR_CONTROLLER_H__

#include "c4d.h"

#include "../scene/camera_export.h"
#include "../scene/scene_exporter.h"
#include "../session/frame_reader.h"
#include "../session/frame_watcher.h"
#include "ipr_worker.h"

namespace cyclesrender
{

enum class IPRSTATE
{
	STOPPED,	///< nothing renders; Blender may still be running, ready for the next start
	STARTING,	///< Blender is being started and receives the scene
	RUNNING,	///< rendering, and following every change of the document
	PAUSED,		///< the render is held and changes of the document wait
	FAILED		///< Blender is gone; GetMessage() says why. Start() begins afresh.
} MAXON_ENUM_LIST(IPRSTATE);

/// Which camera the IPR renders through.
enum class IPRCAMERA
{
	RENDER_VIEW,	///< the camera of the view marked as render view
	AUTO,					///< the camera of the view the user works in, following when that changes
	OBJECT				///< one camera object of the scene
} MAXON_ENUM_LIST(IPRCAMERA);

/// A render pass the IPR can show: the name for the user and Blender's identifier.
struct IprPass
{
	const maxon::Char* label;
	const maxon::Char* id;
};

/// Memory figures for the status bar, in bytes; 0 where unknown.
struct IprMemory
{
	maxon::Int64 ram = 0;
	maxon::Int64 ramTotal = 0;
	maxon::Int64 vram = 0;
	maxon::Int64 vramTotal = 0;
};

//----------------------------------------------------------------------------------------
/// One per Cinema 4D, used from the main thread only. The window calls the commands and,
/// several times a second, Tick(), which is where changes of the document are noticed and
/// sent. All talking to Blender happens on the worker thread; frames come back through
/// shared memory (ReadFrame).
///
/// What is sent after a change is only what changed: every scene item is fingerprinted and
/// compared with what Blender holds (SyncState). Moving the camera does not walk the scene.
///
/// How a change of the document reaches Blender (0.2.0, B11):
/// - A finished edit (MarkSceneChanged: Cinema 4D's EVMSG_CHANGE, a tool's last step, another time):
///   the scene is exported whole at the next Tick(), as since 0.1.0.
/// - A redraw of the views (NoteRedraw: EVMSG_DOCUMENTRECALCULATED, which follows every redraw): the
///   document's change counters decide. Nothing moved (the view was navigated, a manager redrawn):
///   nothing is exported. Something moved: what moved is sent alone if FollowStep() can, else the scene
///   whole.
/// - A step of a change in progress (FollowStep: a tool's drag in a view, a value dragged in the
///   Attribute Manager): with the Preferences' "Optimized Live Updates" only what moves is sent, each
///   item by itself and the same, byte for byte, as a whole export would write it: the camera, the
///   lights, and the place of objects (SceneExporter::WriteObjectAlone). Whatever a step cannot send
///   alone is exported whole, in that step or, inside an evaluation, when the change has ended.
/// - After steps and after redraws that were not exported, the scene is exported whole once more when
///   the change has ended or nothing happened for a moment (the settle export), so that Blender holds
///   exactly what a whole export gives, also where a counter missed a change.
//----------------------------------------------------------------------------------------
class IprController
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(IprController);

public:
	/// Use Get(); the constructor is public only because the SDK's allocator needs it.
	IprController() = default;
	~IprController();

	static IprController& Get();
	/// Ends the worker and Blender. Called when Cinema 4D shuts down.
	static void Free();

	// --- the window's buttons ---
	void Start();
	void Stop();
	void TogglePause();
	/// Sends the whole scene again and restarts the render.
	void Reload();

	void SetCamera(IPRCAMERA mode, cinema::BaseObject* camera = nullptr);
	IPRCAMERA GetCameraMode() const { return _cameraMode; }
	cinema::BaseObject* GetCameraObject(cinema::BaseDocument* doc) const;
	//----------------------------------------------------------------------------------------
	/// The camera the IPR renders through: the chosen camera object, or the scene camera of the view
	/// the camera list names (<Render View>, <Auto>), or that view's editor camera when it has none.
	/// For navigating from the window (ipr_navigation.h).
	/// @param[out] editorView  The view whose editor camera it is; nullptr for a camera object of the document.
	//----------------------------------------------------------------------------------------
	cinema::BaseObject* GetRenderCamera(cinema::BaseDocument* doc, cinema::BaseDraw*& editorView) const;
	/// While locked the render keeps the camera it has, whatever happens to it in Cinema 4D.
	void SetCameraLock(maxon::Bool locked);
	maxon::Bool IsCameraLocked() const { return _cameraLocked; }

	/// The passes of the engine in use, and the one shown.
	maxon::Block<const IprPass> GetPasses() const;
	void SetPass(maxon::Int32 index);
	maxon::Int32 GetPass() const { return _passIndex; }

	/// NOTOK: the engine of the Render Settings; otherwise CYCLES_ENGINE_CYCLES or CYCLES_ENGINE_EEVEE.
	void SetEngineOverride(maxon::Int32 engine);
	maxon::Int32 GetEngineOverride() const { return _engineOverride; }
	/// The engine rendering now (CYCLES_ENGINE_...).
	maxon::Int32 GetEngine() const { return _engine; }

	/// Size of the IPR image relative to the Render Settings resolution (1 = full size).
	/// Used when the image does not follow the window.
	void SetResolutionScale(maxon::Float64 scale);
	maxon::Float64 GetResolutionScale() const { return _scale; }

	/// Fit window: the image has the size of the window's picture area and shows the camera's
	/// picture fitted into it, with more of the scene around it. Otherwise the image is the
	/// camera's picture at the Render Settings resolution times the resolution scale.
	void SetFitWindow(maxon::Bool fit);
	maxon::Bool IsFitWindow() const { return _fitWindow; }
	/// The size of the window's picture area in pixels; 0, 0 while there is no window. A
	/// changing size is followed once it has stood still for a moment.
	void SetWindowSize(maxon::Int32 width, maxon::Int32 height);
	/// The image the IPR renders and where the camera's picture lies in it.
	const CameraFraming& GetFraming() const { return _framing; }
	/// The render region of the Render Settings (Output > Render Region), which the IPR follows:
	/// only that part of the camera's picture is rendered, the rest of the image is black.
	/// In pixels of the IPR image: left, top, right, bottom (right and bottom exclusive).
	/// @return  False when no region is set.
	maxon::Bool GetRegion(maxon::Int32& x0, maxon::Int32& y0, maxon::Int32& x1, maxon::Int32& y1) const;
	/// Width / height of the camera's picture: that of the Render Settings resolution.
	maxon::Float64 GetFrameAspect() const { return _frameAspect; }

	/// Stops the render and ends Blender; the next Start() launches a fresh one.
	void RestartBlender();

	void ShowBlenderWindow(maxon::Bool visible);
	maxon::Bool IsBlenderWindowShown() const { return _windowShown; }

	// --- regular calls ---
	/// The document changed (Cinema 4D's EVMSG_CHANGE), or a change in progress has ended: the scene is
	/// exported whole at the next Tick().
	void MarkSceneChanged();
	//----------------------------------------------------------------------------------------
	/// Cinema 4D evaluated the scene for a redraw (EVMSG_DOCUMENTRECALCULATED). The next Tick() looks at
	/// the document's change counters (GetHDirty and its time): unchanged since the last export or update,
	/// nothing is exported (measured: every redraw of a view sends this message, also when only the view's
	/// own camera moved; before 0.2.0 each cost an export of the whole scene); changed, what moved goes
	/// alone if the Preferences allow and it can (FollowStep), else the scene whole. Either way the scene
	/// is exported whole once things have stood still (the settle export), because the counters do not
	/// see everything (a value of a node material, the document's unit in Cinema 4D 2024).
	//----------------------------------------------------------------------------------------
	void NoteRedraw();
	//----------------------------------------------------------------------------------------
	/// A step of a change in progress: sends what changed since the last export or update, if Blender is
	/// idle. With the Preferences' "Optimized Live Updates" (prefs/settings.h) only what moves is sent,
	/// each item alone:
	/// - the camera, always by itself (SyncCamera);
	/// - every light of the document (its values and its place; SceneExporter::WriteLightAlone);
	/// - the `object` items of objects of the document that moved, and of the objects below them, when
	///   nothing but places changed (SceneExporter::WriteObjectAlone).
	/// The document's objects are compared with what the last whole export saw: the counters of each
	/// object's matrix, data (with its tags') and cache, and its render dot. The step is sent alone only if
	/// every change is one of: a matrix; the data of a light or a camera. It is not when a cache was built
	/// anew (the moved object is the input of a generator, a deformer, the target of a Cloner, ...), when
	/// data of another object or anything else in the document changed (materials, render settings, the
	/// time, the hierarchy), when a moved object or one below it shows something that has no item of its
	/// own place (SceneExporter::GetUnfollowedOwners), or lies below an object that an instance shows
	/// (GetReferenceRoots). Then the scene is exported whole in this step if @p allowWhole, and otherwise
	/// when the change has ended (the document is marked changed).
	/// Without the preference: the scene whole if @p allowWhole (as since F15), else the lights alone (as
	/// since F18).
	/// @param[in] allowWhole  False inside an evaluation of the document (the scene hook), where the
	///                        caches a whole export reads may not be built yet.
	//----------------------------------------------------------------------------------------
	void FollowStep(maxon::Bool allowWhole);
	/// Notices state changes, sends what changed in the document. Cheap when nothing did. While the
	/// timeline plays it follows each frame, unless the Preferences' live updates are off (0.2.0, F22;
	/// prefs/settings.h): then it sends nothing until playback stops.
	void Tick();
	/// Only the camera part of Tick(): sends the camera if it moved and Blender is idle. For
	/// messages that arrive while Cinema 4D is in a modal loop, when the window's timer does not run.
	void FollowCamera();
	//----------------------------------------------------------------------------------------
	/// Only the lights (0.2.0, F18): writes every light of the document the last export went through
	/// again by itself (SceneExporter::WriteLightAlone) and sends those that differ from what Blender
	/// holds, if Blender is idle. No walk of the document, no other item. For a light's value dragged
	/// in the Attribute Manager: Cinema 4D is in a modal loop then, in which the window gets no timer
	/// and no message, but the document is evaluated at every step (ipr_scenehook.h). A light that
	/// is gone, or a new one, is left to the next full export (the document is marked changed).
	//----------------------------------------------------------------------------------------
	void FollowLights();

	IPRSTATE GetState() const { return _state; }
	const maxon::String& GetMessage() const { return _message; }
	/// True while the IPR has failed because no Blender can be started as chosen (none selected or
	/// found, the file is gone, a version that is not supported: GetBlenderProblem()). The window
	/// then offers to open the Preferences instead of starting again (0.2.0, B7).
	maxon::Bool IsBlenderMissing() const { return _state == IPRSTATE::FAILED && _blenderMissing; }

	/// Moves the newest frame into @p frame if this has not returned it before. The window calls
	/// it when the core message ID_CORE_MESSAGE arrives (see FrameWatcher). @p minSeconds: at most one
	/// frame per that many seconds, the last one always (FrameWatcher::Take); 0 for no limit.
	maxon::Bool ReadFrame(Frame& frame, maxon::Float64 minSeconds = 0.0);
	FrameStatus GetStatus() const { return _reader.GetStatus(); }
	/// The size the IPR renders at.
	void GetSize(maxon::Int32& width, maxon::Int32& height) const { width = _width; height = _height; }
	/// Seconds the current render has taken; stands still once it is finished or paused.
	maxon::Float64 GetElapsedSeconds() const;
	IprMemory GetMemory();

	/// What renders, as Blender reported it (IprWorker::GetDeviceInfo): nothing known while no reply came.
	IprDeviceInfo GetDeviceInfo() const { return _worker ? _worker->GetDeviceInfo() : IprDeviceInfo(); }
	//----------------------------------------------------------------------------------------
	/// The device in use as the window's status line says it, from Blender's reply, not from what was
	/// asked for (0.2.0, B11): "Cycles on OptiX: NVIDIA GeForce RTX 4090", "Cycles on CPU, 30 threads (Render Settings)"
	/// (or "(no GPU found)" when GPU Compute or Automatic ended on the CPU),
	/// "Eevee on NVIDIA GeForce RTX 4090". Empty while nothing renders or no reply has come.
	//----------------------------------------------------------------------------------------
	maxon::String GetDeviceText() const;
	/// The priority classes of Cinema 4D and of the IPR's Blender as last seen and set (FollowPriority),
	/// as ranks of platform/process_stats.h; -1 while not known. For tests.
	maxon::Int32 GetOwnPriority() const { return _ownPriority; }
	maxon::Int32 GetBlenderPriority() const { return _blenderPriority; }
	/// The process id of the IPR's Blender; 0 while none runs.
	maxon::Int GetBlenderProcessId() const { return _processId; }
	/// The token of the Blender whose frames are read (names its frame block); empty while none runs.
	const maxon::String& GetSessionToken() const { return _readerToken; }

	/// Tell the controller that @p frame is now on screen. It measures how long a change of the
	/// document took to change the picture.
	void NoteFrame(const Frame& frame);
	/// Milliseconds from the last change of the document (or camera) that was sent to Blender
	/// until the picture on screen differed. 0 before the first measurement.
	maxon::Float64 GetLastLatencyMilliseconds() const { return _latencyMs; }
	/// The same time in four parts: until the change was exported and handed to the worker,
	/// until Blender had applied it, until the changed frame arrived from Blender, until it
	/// was on screen.
	const maxon::Float64* GetLastLatencyParts() const { return _latencyParts; }

	// --- for tests ---
	maxon::Int32 GetSentSceneCount() const { return _worker ? _worker->GetSentSceneCount() : 0; }
	maxon::Int32 GetSentRequestCount() const { return _worker ? _worker->GetSentRequestCount() : 0; }
	maxon::Int32 GetLastChangeCount() const { return _lastChangeCount; }
	/// What the last export did with the meshes, and how long it took (export and comparison).
	const ExportStats& GetLastExportStats() const { return _lastStats; }
	maxon::Float64 GetLastExportMilliseconds() const { return _lastExportMilliseconds; }
	/// How long the last FollowLights() took to write the lights and compare them (not the sending).
	maxon::Float64 GetLastLightsMilliseconds() const { return _lastLightsMilliseconds; }
	/// The number of lights FollowLights() writes (those of the last full export that are objects of the document).
	maxon::Int GetFollowedLightCount() const { return _lights.GetCount(); }
	/// The number of `object` items FollowStep() can write alone, and of objects of the document it compares.
	maxon::Int GetFollowedObjectCount() const { return _objects.GetCount(); }
	maxon::Int GetFollowedNodeCount() const { return _nodesValid ? _nodes.GetCount() : 0; }
	/// Since the IPR was started: redraws that exported nothing because no counter had moved, steps and
	/// redraws answered with an update of single items, and settle exports.
	maxon::Int32 GetGatedCount() const { return _gatedCount; }
	maxon::Int32 GetAloneCount() const { return _aloneCount; }
	maxon::Int32 GetSettleCount() const { return _settleCount; }
	/// Of the last FollowStep(): how long it took to compare and write (not the sending), how many items
	/// of objects it wrote, and whether it found that the scene has to be exported whole.
	maxon::Float64 GetLastAloneMilliseconds() const { return _lastAloneMilliseconds; }
	maxon::Int32 GetLastAloneObjects() const { return _lastAloneObjects; }
	maxon::Bool WasWholeNeeded() const { return _wholeNeeded; }
	/// A whole export is still to come once things stand still (after steps and redraws without one).
	maxon::Bool IsSettlePending() const { return _settlePending; }
	maxon::Bool IsIdle() const { return !_worker || _worker->GetPendingCount() == 0; }
	void KillBlenderForTest();
	/// Exports the active document whole, as from scratch (without reusing a mesh), with the settings
	/// of the last export, and compares every item with what Blender holds as far as the IPR knows:
	/// JSON {"items": n, "differences": [ids]}. Nothing is sent and nothing remembered changes. For
	/// tests: no difference means Blender holds exactly what a full export would give it.
	maxon::Result<maxon::String> VerifySync();

private:
	maxon::Result<void> EnsureWorker();
	/// Sends settings, size and every scene item that changed. @p clear: start from an empty Blender scene.
	maxon::Result<void> SyncScene(maxon::Bool clear);
	/// Works out the image size and sends it to Blender if it changed.
	void SyncSize(cinema::BaseDocument* doc);
	/// Works out the render region inside the image and sends it to Blender if it changed.
	void SyncRegion(cinema::BaseDocument* doc);
	/// Sends the camera if it differs from the one Blender has.
	maxon::Result<void> SyncCamera(cinema::BaseDocument* doc);
	/// What a tick does when the scene is not exported: the image size if the window's changed, and the camera.
	maxon::Result<void> SyncView(cinema::BaseDocument* doc);
	/// FollowLights() without its checks.
	maxon::Result<void> SyncLights(cinema::BaseDocument* doc);
	/// Remembers the lights of the export that was just written (FollowLights).
	maxon::Result<void> NoteLights(SceneExporter& exporter);
	/// The document's change counters as the gate and FollowStep() compare them.
	struct DocumentCounters
	{
		maxon::UInt32 all = 0;        ///< GetHDirty(ALL)
		cinema::BaseTime time;
		maxon::Bool known = false;
		maxon::Bool operator ==(const DocumentCounters& other) const { return known && other.known && all == other.all && time == other.time; }
	};
	static DocumentCounters ReadCounters(cinema::BaseDocument* doc);
	/// GetHDirty of everything but the objects' data, their matrices and the tags, which FollowStep()
	/// looks at object by object.
	static maxon::UInt32 ReadOtherCounters(cinema::BaseDocument* doc);
	/// Forgets what FollowStep() and the gate compare with (Blender's scene is no longer known).
	void ForgetFollowed();
	/// Remembers the objects of the document as the export that was just written saw them, and the items
	/// that can be written alone (FollowStep).
	maxon::Result<void> NoteFollowed(cinema::BaseDocument* doc, SceneExporter& exporter);
	/// FollowStep()'s update of single items. @return False when the scene has to be exported whole
	/// (nothing was sent then).
	maxon::Result<maxon::Bool> SyncAlone(cinema::BaseDocument* doc, const DocumentCounters& counters);
	/// What Tick() does after a redraw (NoteRedraw).
	maxon::Result<void> FollowRedraw(cinema::BaseDocument* doc);
	cinema::BaseObject* ResolveCamera(cinema::BaseDocument* doc) const;
	void PostSimple(const maxon::Char* op, const maxon::Char* key = nullptr, maxon::Bool value = false);
	void PostPass();
	/// Drops everything known about what Blender holds and shows @p message in the window.
	void Fail(const maxon::String& message);
	/// Prints a warning to the console, once per start of the IPR.
	void ReportOnce(const maxon::String& warning);
	/// Prints the warnings the worker collected from Blender's replies.
	void ReportWarnings();
	//----------------------------------------------------------------------------------------
	/// Keeps the IPR's Blender one priority class below Cinema 4D's current class, but not below "below
	/// normal" (0.2.0, B11): below normal while Cinema 4D runs at normal or below normal (its default, its
	/// preference "lower process priority"), above normal under high, and so on. Not idle, which stalls
	/// Blender entirely while Cinema 4D keeps every core busy (BlenderPriorityRank in the .cpp). Looks at
	/// Cinema 4D's class once a second and sets Blender's again when it changed or Blender is new. Cinema
	/// 4D's own class is never changed.
	//----------------------------------------------------------------------------------------
	void FollowPriority();
	/// Starts measuring how long a change takes to show, unless a measurement is running.
	/// @param[in] changedSince  When the change happened (Unix seconds); 0: now.
	void ArmLatency(maxon::Float64 changedSince);

	IprWorker* _worker = nullptr;
	FrameReader _reader;									///< for the render progress; pixels come through the watcher
	FrameWatcher* _watcher = nullptr;
	maxon::String _readerToken;
	maxon::Int _processId = 0;

	IPRSTATE _state = IPRSTATE::STOPPED;
	maxon::String _message;
	maxon::Bool _blenderMissing = false;	///< IsBlenderMissing()
	maxon::Bool _sceneDirty = false;
	maxon::Bool _viewStarted = false;			///< Blender's viewport is in Rendered mode
	const cinema::BaseDocument* _document = nullptr;	///< the document Blender holds; compared, never dereferenced

	SyncState _sceneState;
	SyncState _cameraState;
	/// A light of the last export, found again through a link (FollowLights).
	struct FollowedLight
	{
		FollowedLight() = default;
		FollowedLight(FollowedLight&& other) : link(other.link), id(std::move(other.id)), linked(other.linked), linking(std::move(other.linking)) { other.link = nullptr; }
		FollowedLight& operator =(FollowedLight&& other)
		{
			if (this != &other)
			{
				cinema::BaseLink::Free(link);
				link = other.link;
				other.link = nullptr;
				id = std::move(other.id);
				linked = other.linked;
				linking = std::move(other.linking);
			}
			return *this;
		}
		~FollowedLight() { cinema::BaseLink::Free(link); }
		cinema::BaseLink* link = nullptr;
		maxon::String id;
		maxon::Bool linked = false;
		LightLinking linking;
	};
	maxon::BaseArray<FollowedLight> _lights;
	/// An object of the document as the last whole export saw it, in the order of a walk from the top
	/// (each object before the objects below it): what FollowStep() compares.
	struct FollowedNode
	{
		const cinema::BaseObject* object = nullptr;	///< compared with the document's, never dereferenced
		maxon::UInt32 matrixDirty = 0;
		maxon::UInt32 dataDirty = 0;			///< the object's and its tags'
		maxon::UInt32 cacheDirty = 0;			///< its cache counter; with `deformed`, the counters of everything in its cache
		maxon::Int32 renderMode = 0;
		maxon::Int32 end = 0;							///< index after the last object below it
		maxon::Int32 firstObject = 0;			///< its items in _objects
		maxon::Int32 objectCount = 0;
		maxon::Bool dataAlone = false;		///< a change of its data needs no whole export: a light (written alone) or a camera (SyncCamera)
		maxon::Bool unfollowed = false;		///< shows something whose place cannot be written alone
		maxon::Bool referenced = false;		///< below an object an instance shows
		maxon::Bool deformed = false;			///< shows a deformed mesh (SceneExporter::GetDeformedOwners)
	};
	maxon::BaseArray<FollowedNode> _nodes;
	maxon::BaseArray<WrittenObject> _objects;	///< the `object` items that can be written alone, grouped by their owner's node
	maxon::Bool _nodesValid = false;
	DocumentCounters _counters;						///< at the last export or update
	maxon::UInt32 _otherCounters = 0;			///< ReadOtherCounters() at the last whole export
	maxon::Bool _redrawn = false;					///< NoteRedraw() since the last Tick()
	maxon::Bool _settlePending = false;
	maxon::Float64 _activityTime = 0.0;		///< when the document last may have changed (UnixTime)
	maxon::Int32 _gatedCount = 0;
	maxon::Int32 _aloneCount = 0;
	maxon::Int32 _settleCount = 0;
	maxon::Float64 _lastAloneMilliseconds = 0.0;
	maxon::Int32 _lastAloneObjects = 0;
	maxon::Bool _wholeNeeded = false;
	maxon::String _sentView = "?"_s;			///< the last `view_camera` request; empty: Blender looks through its camera object; "?": not known
	maxon::String _sentSettings;
	ExportSettings _exportSettings;
	maxon::Int32 _lastChangeCount = 0;
	ExportStats _lastStats;
	maxon::Float64 _lastExportMilliseconds = 0.0;
	maxon::Float64 _lastLightsMilliseconds = 0.0;
	cinema::BaseTime _exportedTime;							///< the document's time at the last export (playback sends no change event)
	maxon::Bool _exportedTimeKnown = false;

	maxon::Int32 _width = 0;
	maxon::Int32 _height = 0;
	maxon::Float64 _scale = 1.0;
	maxon::Bool _fitWindow = true;
	maxon::Int32 _windowWidth = 0;				///< the window's picture area, as last reported
	maxon::Int32 _windowHeight = 0;
	maxon::Float64 _windowSizeTime = 0.0;	///< when that size was first reported
	maxon::Bool _sizeDirty = false;				///< the image size may have to follow
	maxon::Float64 _frameAspect = 16.0 / 9.0;
	CameraFraming _framing;
	maxon::Bool _hasRegion = false;
	maxon::Float64 _region[4] = { 0.0, 0.0, 1.0, 1.0 };	///< fractions of the image from its lower left corner: min x, min y, max x, max y
	maxon::String _sentRegion = "?"_s;		///< the last `view_region` request; "?": not known
	maxon::Int32 _engineOverride = NOTOK;
	maxon::Int32 _engine = 0;
	maxon::Int32 _passIndex = 0;
	IPRCAMERA _cameraMode = IPRCAMERA::RENDER_VIEW;
	cinema::AutoAlloc<cinema::BaseLink> _cameraLink;
	maxon::Bool _cameraLocked = false;
	maxon::Bool _hasLockedCamera = false;
	CameraValues _lockedCamera;						///< while locked: the camera as it was
	maxon::Bool _windowShown = false;

	maxon::BaseArray<maxon::String> _reportedWarnings;
	maxon::Float64 _dirtySince = 0.0;			///< when the document was first reported changed since the last export
	maxon::Float64 _changeTime = 0.0;			///< start of the latency measurement in progress
	maxon::Bool _latencyArmed = false;
	maxon::UInt64 _frameChecksum = 0;			///< of the frame on screen
	maxon::UInt64 _armedChecksum = 0;			///< of the frame that was on screen when the change was sent
	maxon::Float64 _latencyMs = 0.0;
	maxon::Float64 _postTime = 0.0;				///< when that change was handed to the worker
	maxon::Float64 _latencyParts[4] = { 0.0, 0.0, 0.0, 0.0 };
	maxon::Float64 _frameArrival = 0.0;		///< when the frame last returned by ReadFrame() arrived
	IprMemory _memory;
	maxon::Float64 _memoryTime = 0.0;
	maxon::Int _priorityProcess = 0;			///< the Blender whose class FollowPriority() set last
	maxon::Int32 _ownPriority = -1;				///< Cinema 4D's class then
	maxon::Int32 _blenderPriority = -1;		///< the class given to Blender
	maxon::Float64 _priorityTime = 0.0;		///< when FollowPriority() last looked
	maxon::Int32 _askedDevice = 0;				///< the Render Settings' device last sent (CYCLES_DEVICE_...), for GetDeviceText()
};

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_CONTROLLER_H__
