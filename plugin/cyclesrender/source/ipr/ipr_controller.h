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
	/// The document changed (Cinema 4D's EVMSG_CHANGE).
	void MarkSceneChanged();
	/// Notices state changes, sends what changed in the document. Cheap when nothing did.
	void Tick();
	/// Only the camera part of Tick(): sends the camera if it moved and Blender is idle. For
	/// messages that arrive while Cinema 4D is in a modal loop, when the window's timer does not run.
	void FollowCamera();

	IPRSTATE GetState() const { return _state; }
	const maxon::String& GetMessage() const { return _message; }

	/// Moves the newest frame into @p frame if this has not returned it before. The window calls
	/// it when the core message ID_CORE_MESSAGE arrives (see FrameWatcher).
	maxon::Bool ReadFrame(Frame& frame);
	FrameStatus GetStatus() const { return _reader.GetStatus(); }
	/// The size the IPR renders at.
	void GetSize(maxon::Int32& width, maxon::Int32& height) const { width = _width; height = _height; }
	/// Seconds the current render has taken; stands still once it is finished or paused.
	maxon::Float64 GetElapsedSeconds() const;
	IprMemory GetMemory();

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
	cinema::BaseObject* ResolveCamera(cinema::BaseDocument* doc) const;
	void PostSimple(const maxon::Char* op, const maxon::Char* key = nullptr, maxon::Bool value = false);
	void PostPass();
	/// Drops everything known about what Blender holds and shows @p message in the window.
	void Fail(const maxon::String& message);
	/// Prints a warning to the console, once per start of the IPR.
	void ReportOnce(const maxon::String& warning);
	/// Prints the warnings the worker collected from Blender's replies.
	void ReportWarnings();
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
	maxon::Bool _sceneDirty = false;
	maxon::Bool _viewStarted = false;			///< Blender's viewport is in Rendered mode
	const cinema::BaseDocument* _document = nullptr;	///< the document Blender holds; compared, never dereferenced

	SyncState _sceneState;
	SyncState _cameraState;
	maxon::String _sentView = "?"_s;			///< the last `view_camera` request; empty: Blender looks through its camera object; "?": not known
	maxon::String _sentSettings;
	ExportSettings _exportSettings;
	maxon::Int32 _lastChangeCount = 0;
	ExportStats _lastStats;
	maxon::Float64 _lastExportMilliseconds = 0.0;
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
};

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_CONTROLLER_H__
