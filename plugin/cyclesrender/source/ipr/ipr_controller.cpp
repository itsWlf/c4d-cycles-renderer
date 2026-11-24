// SPDX-License-Identifier: Apache-2.0
// The IPR's state machine and its delta sync (ipr_controller.h).
//
// Reading order: the commands the window's buttons call (Start, Stop, TogglePause, Reload),
// then "following the document". Tick() is called by the window's timer and decides what to
// send: SyncScene() after a change of the document, FollowRedraw() after a redraw, SyncCamera() and
// SyncSize() on every tick. Then "a change in progress": the gate of the change counters and the steps
// that send what moved alone (FollowStep, SyncAlone), and last the results the window reads. Nothing
// here waits for Blender: requests are posted to the worker thread (ipr_worker.h), and frames come back
// through shared memory (session/frame_watcher.h).

#include "c4d.h"

#include "vpcyclesrender.h"

#include "../ids.h"
#include "../platform/process_stats.h"
#include "../platform/shared_memory.h"
#include "../prefs/devices.h"
#include "../prefs/settings.h"
#include "../scene/camera_export.h"
#include "../videopost/output_settings.h"
#include "../videopost/render_settings_map.h"
#include "ipr_controller.h"
#include "ipr_scenehook.h"
#include "ipr_trace.h"

namespace cyclesrender
{

// The passes each engine's viewport can show (docs/reference/blender.md, "Viewport controls").
static const IprPass CYCLES_PASSES[] =
{
	{ "Beauty Pass", "COMBINED" },
	{ "Diffuse Direct", "DIFFUSE_DIRECT" },
	{ "Diffuse Indirect", "DIFFUSE_INDIRECT" },
	{ "Diffuse Color", "DIFFUSE_COLOR" },
	{ "Glossy Direct", "GLOSSY_DIRECT" },
	{ "Glossy Indirect", "GLOSSY_INDIRECT" },
	{ "Glossy Color", "GLOSSY_COLOR" },
	{ "Transmission Direct", "TRANSMISSION_DIRECT" },
	{ "Transmission Indirect", "TRANSMISSION_INDIRECT" },
	{ "Transmission Color", "TRANSMISSION_COLOR" },
	{ "Volume Direct", "VOLUME_DIRECT" },
	{ "Volume Indirect", "VOLUME_INDIRECT" },
	{ "Emission", "EMISSION" },
	{ "Background", "BACKGROUND" },
	{ "Ambient Occlusion", "AO" },
	{ "Shadow Catcher", "SHADOW_CATCHER" },
	{ "Position", "POSITION" },
	{ "Normal", "NORMAL" },
	{ "UV", "UV" },
	{ "Mist", "MIST" },
	{ "Denoising Albedo", "DENOISING_ALBEDO" },
	{ "Denoising Normal", "DENOISING_NORMAL" },
	{ "Sample Count", "SAMPLE_COUNT" },
};

static const IprPass EEVEE_PASSES[] =
{
	{ "Beauty Pass", "COMBINED" },
	{ "Diffuse Light", "DIFFUSE_LIGHT" },
	{ "Diffuse Color", "DIFFUSE_COLOR" },
	{ "Specular Light", "SPECULAR_LIGHT" },
	{ "Specular Color", "SPECULAR_COLOR" },
	{ "Volume Light", "VOLUME_LIGHT" },
	{ "Emission", "EMISSION" },
	{ "Environment", "ENVIRONMENT" },
	{ "Ambient Occlusion", "AO" },
	{ "Shadow", "SHADOW" },
	{ "Transparent", "TRANSPARENT" },
	{ "Position", "POSITION" },
	{ "Normal", "NORMAL" },
	{ "Mist", "MIST" },
};

// The IPR image is a hidden Blender window of this size; these limits keep it usable.
static const maxon::Int32 MIN_SIDE = 16;
static const maxon::Int32 MAX_SIDE = 8192;

// A window being resized reports a new size many times a second, and every new size costs
// Blender a new frame buffer and a restarted render. The image follows once the size has
// stood still this long.
static const maxon::Float64 WINDOW_SIZE_SETTLE_SECONDS = 0.12;

// After steps sent alone and after redraws that exported nothing, the scene is exported whole once
// nothing has happened for this long (the settle export; a change event or a tool's last step brings it
// at once). Long enough that a redraw of every frame of a slow view (4 a second) does not trigger it
// between two frames, short enough to be no wait.
static const maxon::Float64 SETTLE_SECONDS = 0.25;

// A step of a change in progress that collects objects again (IprController::SyncAlone) exports their
// meshes on the thread that makes the step: the main thread, or the thread that evaluates the document
// (IprController::FollowEvaluation). One that took longer than the first number is
// followed by its own length times the second without another: such steps then take at most a quarter
// of the time, however heavy the mesh. Measured: docs/reference/c4d-sdk.md, "What the IPR sends while a
// change is in progress".
static const maxon::Float64 MESH_STEP_FREE_MILLISECONDS = 4.0;
static const maxon::Float64 MESH_STEP_QUIET_FACTOR = 3.0;

// The one controller: written on the main thread (Get, Free), read there and, through
// FollowEvaluation(), on a thread that evaluates the document.
static maxon::AtomicPtr<IprController> g_controller;
// The threads that are in FollowEvaluation() at this moment. Each counts itself before it reads
// g_controller, and Free() takes the pointer away before it reads the count: so a thread that still
// found the controller is seen by Free(), which waits for it, and one that comes later finds none.
static maxon::AtomicInt32 g_evaluating;

// A Cycles render on the CPU in the IPR leaves the Preferences' "CPU Threads Left Free" to Cinema 4D
// (prefs/settings.h): a fixed thread count of the logical processors less those, or the
// Render Settings' own fixed count if that is smaller. Only for the IPR: final frames use the machine.
static void LeaveThreadsFree(cinema::BaseContainer& data)
{
	const maxon::Int32 free = GetIprThreadsFree();
	if (free <= 0)
		return;	// the Render Settings as they are
	const maxon::Int32 limit = maxon::Max(maxon::Int32(1), maxon::Int32(platform::GetLogicalProcessorCount()) - free);
	const maxon::Bool fixed = data.GetInt32(CYCLES_THREADS_MODE, CYCLES_THREADS_MODE_AUTO) == CYCLES_THREADS_MODE_FIXED;
	const maxon::Int32 threads = fixed ? maxon::ClampValue(data.GetInt32(CYCLES_THREADS, limit), maxon::Int32(1), limit) : limit;
	data.SetInt32(CYCLES_THREADS_MODE, CYCLES_THREADS_MODE_FIXED);
	data.SetInt32(CYCLES_THREADS, threads);
}

// The priority class of the IPR's Blender for Cinema 4D's class (ranks of platform/process_stats.h):
// one below, but not below "below normal" unless Cinema 4D itself is there. Not idle: a Blender at idle
// stands completely still while Cinema 4D's threads keep every core busy, no reply and no frame for as
// long as that lasts, on the GPU too (measured: no request answered within 10 s, also with Blender's
// main thread raised; docs/reference/c4d-sdk.md, "Priority and cores of the IPR's Blender"), and the IPR's worker would
// wait for it. So with Cinema 4D's default (below normal) both share the class, and the threads a CPU render
// leaves free (GetIprThreadsFree) keep cores for Cinema 4D; with Cinema 4D at normal Blender is below it.
static maxon::Int32 BlenderPriorityRank(maxon::Int32 own)
{
	return own >= 2 ? own - 1 : own;
}

// A value that differs from thread to thread and is never 0: the address of a variable each thread has
// its own copy of.
static maxon::Int64 ThisThread()
{
	static thread_local maxon::Char marker = 0;
	return maxon::Int64(reinterpret_cast<maxon::UInt>(&marker));
}

maxon::Bool IprLock::TryLock()
{
	const maxon::Int64 me = ThisThread();
	if (_owner.Load() != me && !_owner.TryCompareAndSwap(me, 0))
		return false;
	++_depth;
	return true;
}

void IprLock::Lock()
{
	// Held by the other thread for one step at most: well under a millisecond, or as long as a heavy
	// mesh takes to export. Looking again and again first, then with pauses.
	const maxon::TimeValue began = maxon::TimeValue::GetTime();
	while (!TryLock())
	{
		if ((maxon::TimeValue::GetTime() - began).GetMilliseconds() > 1.0)
			cinema::GeSleep(1);
	}
}

void IprLock::Unlock()
{
	if (--_depth == 0)
		_owner.Set(0);
}

IprController& IprController::Get()
{
	IprController* controller = g_controller.Load();
	if (!controller)
	{
		iferr (controller = NewObj(IprController))
			CriticalStop("Out of memory for the IPR controller.");
		g_controller.Set(controller);
	}
	return *controller;
}

void IprController::Free()
{
	// No other thread finds it from now on. One that found it before is in FollowEvaluation() and
	// counted there: it is waited for. It leaves by itself, because nothing in a step waits for the main
	// thread, and it holds the controller for one step at most (IprLock::Lock).
	IprController* controller = g_controller.Swap(nullptr);
	if (!controller)
		return;
	while (g_evaluating.Load() != 0)
		cinema::GeSleep(1);
	DeleteObj(controller);
}

maxon::Bool IprController::FollowEvaluation(cinema::BaseDocument* doc, maxon::Bool optimized)
{
	// Counted first, read second (g_evaluating).
	g_evaluating.SwapIncrement();
	finally { g_evaluating.SwapDecrement(); };
	IprController* const controller = g_controller.Load();
	return controller && controller->FollowEvaluated(doc, optimized);
}

IprController::~IprController()
{
	DeleteObj(_watcher);
	_reader.Close();
	DeleteObj(_worker);	// ends the thread and Blender
	ForgetWorld();
}

void IprController::ForgetWorld()
{
	for (cinema::BaseLink*& link : _worldLinks)
		cinema::BaseLink::Free(link);
	_worldAlone = false;
	_writtenWorld = SceneExporter::WrittenWorld();
}

maxon::Result<void> IprController::EnsureWorker()
{
	iferr_scope;
	if (!_watcher)
	{
		_watcher = NewObj(FrameWatcher) iferr_return;
		// In a modal loop, where the core message waits, a frame reaches the window this way.
		_watcher->SetArrivalCallback(&NoteIprFrameArrived);
		if (!_watcher->Start())
		{
			DeleteObj(_watcher);
			return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "The frame thread of the IPR could not be started."_s);
		}
	}
	if (_worker)
		return maxon::OK;
	_worker = NewObj(IprWorker) iferr_return;
	if (!_worker->Start())
	{
		DeleteObj(_worker);
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "The IPR's worker thread could not be started."_s);
	}
	return maxon::OK;
}

void IprController::PostSimple(const maxon::Char* op, const maxon::Char* key, maxon::Bool value)
{
	if (!_worker)
		return;
	JsonWriter request;
	request.BeginObject();
	request.String("op", maxon::String(op));
	if (key)
		request.Bool(key, value);
	_worker->PostRequest(request);
}

void IprController::PostPass()
{
	if (!_worker)
		return;
	const maxon::Block<const IprPass> passes = GetPasses();
	if (_passIndex < 0 || _passIndex >= passes.GetCount())
		_passIndex = 0;
	JsonWriter request;
	request.BeginObject();
	request.String("op", "view_pass"_s);
	request.String("pass", maxon::String(passes[_passIndex].id));
	_worker->PostRequest(request);
}

void IprController::Fail(const maxon::String& message)
{
	if (!cinema::GeIsMainThread())
	{
		// The window reads the state and the message on the main thread: Tick() does the rest there.
		_deferredFailure = message;
		return;
	}
	_state = IPRSTATE::FAILED;
	_message = message;
	_viewStarted = false;
	_exportedTimeKnown = false;
	_sceneState.Reset();
	_cameraState.Reset();
	_lights.Reset();
	_empties.Reset();
	ForgetWorld();
	ForgetFollowed();
	_sentView = "?"_s;
	_sentRegion = "?"_s;
	_sentSettings = maxon::String();
	ApplicationOutput("Cycles Render IPR: @", message);
}

void IprController::ReportOnce(const maxon::String& warning)
{
	if (_reportedWarnings.Contains(warning))
		return;
	_reportedWarnings.Append(warning) iferr_ignore("Repeating a warning is harmless.");
	ApplicationOutput("Cycles Render: @", warning);
}

void IprController::ReportWarnings()
{
	if (!_worker)
		return;
	maxon::BaseArray<maxon::String> warnings;
	_worker->TakeWarnings(warnings);
	for (const maxon::String& warning : warnings)
		ReportOnce(warning);
}

void IprController::ArmLatency(maxon::Float64 changedSince)
{
	if (_latencyArmed)
		return;
	_latencyArmed = true;
	_postTime = platform::UnixTime();
	_changeTime = changedSince > 0.0 ? changedSince : _postTime;
	_armedChecksum = _frameChecksum;
}

// --- commands -------------------------------------------------------------------------------

void IprController::Start()
{
	const IprLocked held(_lock);
	if (_state != IPRSTATE::STOPPED && _state != IPRSTATE::FAILED)
		return;

	// Before anything is started: a Blender that cannot be started as chosen is said as such, with
	// where to choose another. The worker would find the same (BlenderProcess::Start).
	_blenderMissing = false;
	const maxon::String problem = GetBlenderProblem();
	if (problem.IsPopulated())
	{
		Fail(problem);
		_blenderMissing = true;
		return;
	}

	iferr (EnsureWorker())
	{
		Fail(err.GetMessage());
		return;
	}
	_worker->ClearFailure();
	_message = maxon::String();
	_reportedWarnings.Reset();

	iferr (IprTask* const open = NewObj(IprTask))
		return;
	open->kind = IprTask::KIND::OPEN;
	_worker->Post(open);

	_state = IPRSTATE::STARTING;
	_viewStarted = false;
	_sceneState.Reset();
	_cameraState.Reset();
	ForgetFollowed();
	_gatedCount = 0;
	_aloneCount = 0;
	_settleCount = 0;
	_sentView = "?"_s;
	_sentRegion = "?"_s;
	_sentSettings = maxon::String();

	iferr (SyncScene(true))
	{
		Fail(err.GetMessage());
		return;
	}
	PostPass();

	JsonWriter start;
	start.BeginObject();
	start.String("op", "view_start"_s);
	start.Int("width", _width);
	start.Int("height", _height);
	_worker->PostRequest(start);
	_viewStarted = true;
	if (_windowShown)
		PostSimple("view_window", "visible", true);
}

void IprController::Stop()
{
	const IprLocked held(_lock);
	if (_state == IPRSTATE::STOPPED)
		return;
	if (_state != IPRSTATE::FAILED && _worker)
	{
		PostSimple("view_stop");
		// Blender stays running for a quick next start, but gives the scene's memory back.
		iferr (IprTask* const clear = NewObj(IprTask))
			return;
		clear->kind = IprTask::KIND::SCENE;
		iferr (clear->scene = NewObj(SceneWriter, true))
		{
			// Without it the task does nothing.
		}
		_worker->Post(clear);
	}
	_state = IPRSTATE::STOPPED;
	_message = maxon::String();
	_viewStarted = false;
	_exportedTimeKnown = false;
	_sceneState.Reset();
	_cameraState.Reset();
	_lights.Reset();
	_empties.Reset();
	ForgetWorld();
	ForgetFollowed();
	_sentView = "?"_s;
	_sentRegion = "?"_s;
	_sentSettings = maxon::String();
}

void IprController::TogglePause()
{
	const IprLocked held(_lock);
	if (_state == IPRSTATE::RUNNING)
	{
		PostSimple("view_pause", "paused", true);
		_state = IPRSTATE::PAUSED;
	}
	else if (_state == IPRSTATE::PAUSED)
	{
		PostSimple("view_pause", "paused", false);
		_state = IPRSTATE::RUNNING;
		_sceneDirty = true;	// whatever changed while paused is sent now
	}
}

void IprController::Reload()
{
	const IprLocked held(_lock);
	if (_state == IPRSTATE::STOPPED || _state == IPRSTATE::FAILED)
	{
		Start();
		return;
	}
	if (_state == IPRSTATE::PAUSED)
	{
		PostSimple("view_pause", "paused", false);
		_state = IPRSTATE::RUNNING;
	}
	// The manual mode, with changes waiting: the whole export that sends what differs. Else from scratch.
	const maxon::Bool differs = _manual && _refreshWaiting && _nodesValid;
	if (!differs)
		_sentSettings = maxon::String();
	iferr (SyncScene(!differs))
		Fail(err.GetMessage());
}

void IprController::SetCamera(IPRCAMERA mode, cinema::BaseObject* camera)
{
	const IprLocked held(_lock);
	_cameraMode = camera || mode != IPRCAMERA::OBJECT ? mode : IPRCAMERA::RENDER_VIEW;
	if (_cameraLink)
		_cameraLink->SetLink(_cameraMode == IPRCAMERA::OBJECT ? camera : nullptr);
	// Another camera may count now: the evaluating thread follows none until the main thread has looked.
	_notedCameraDocument = nullptr;
}

cinema::BaseObject* IprController::GetCameraObject(cinema::BaseDocument* doc) const
{
	const IprLocked held(_lock);
	if (_cameraMode != IPRCAMERA::OBJECT || !_cameraLink || !doc)
		return nullptr;
	return static_cast<cinema::BaseObject*>(_cameraLink->GetLink(doc, Obase));
}

void IprController::SetCameraLock(maxon::Bool locked)
{
	const IprLocked held(_lock);
	if (locked == _cameraLocked)
		return;
	_cameraLocked = locked;
	_hasLockedCamera = false;
	if (!locked)
		return;

	// The lock keeps the camera's values as they are now. The render goes on using them, so
	// the view stands still while its framing can still follow the window's size.
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	const cinema::BaseObject* const camera = doc ? ResolveCamera(doc) : nullptr;
	if (camera)
	{
		_lockedCamera = CameraValues::Read(camera);
		iferr (_lockedImager = ImagerChain::Read(camera))
			_lockedImager = ImagerChain();
		_hasLockedCamera = true;
	}
}

cinema::BaseObject* IprController::ResolveCamera(cinema::BaseDocument* doc) const
{
	cinema::BaseDraw* view = nullptr;
	return GetRenderCamera(doc, view);
}

cinema::BaseObject* IprController::GetRenderCamera(cinema::BaseDocument* doc, cinema::BaseDraw*& editorView) const
{
	const IprLocked held(_lock);
	editorView = nullptr;
	if (!doc)
		return nullptr;
	if (_cameraMode == IPRCAMERA::OBJECT)
	{
		if (cinema::BaseObject* const camera = GetCameraObject(doc))
			return camera;
	}
	// <Auto>: the active view; the render view otherwise, and when there is no active view
	// (FindRenderCamera).
	cinema::BaseDraw* view = _cameraMode == IPRCAMERA::AUTO ? doc->GetActiveBaseDraw() : nullptr;
	if (!view)
		view = doc->GetRenderBaseDraw();
	if (!view)
		return nullptr;
	cinema::BaseObject* const editorCamera = view->GetEditorCamera();
	cinema::BaseObject* const sceneCamera = view->GetSceneCamera(doc);
	if (sceneCamera && sceneCamera != editorCamera)
		return sceneCamera;
	editorView = view;
	return editorCamera;
}

const cinema::BaseObject* IprController::FollowedCamera(cinema::BaseDocument* doc)
{
	if (cinema::GeIsMainThread())
	{
		cinema::BaseDraw* editorView = nullptr;
		cinema::BaseObject* const camera = GetRenderCamera(doc, editorView);
		// What the evaluating thread may read: an object of the document, found again through a link.
		const maxon::Bool ofDocument = camera && !editorView && _notedCamera && camera->GetDocument() == doc;
		if (_notedCamera)
			_notedCamera->SetLink(ofDocument ? camera : nullptr);
		_notedCameraDocument = ofDocument ? doc : nullptr;
		return camera;
	}
	// Which view counts and which camera it shows is the main thread's to ask.
	if (!_notedCamera || !_notedCameraDocument || _notedCameraDocument != doc)
		return nullptr;
	// Through the link: a camera deleted since is not found. (And one taken out of the document but
	// still held by somebody, an undo step or a script, is not read either.)
	const cinema::BaseObject* const camera = static_cast<const cinema::BaseObject*>(_notedCamera->GetLink(doc, Obase));
	return camera && camera->GetDocument() == doc ? camera : nullptr;
}

maxon::Block<const IprPass> IprController::GetPasses() const
{
	if (_engine == CYCLES_ENGINE_EEVEE)
		return maxon::Block<const IprPass>(EEVEE_PASSES, SIZEOF(EEVEE_PASSES) / SIZEOF(EEVEE_PASSES[0]));
	return maxon::Block<const IprPass>(CYCLES_PASSES, SIZEOF(CYCLES_PASSES) / SIZEOF(CYCLES_PASSES[0]));
}

void IprController::SetPass(maxon::Int32 index)
{
	const IprLocked held(_lock);
	_passIndex = index;
	if (_viewStarted)
		PostPass();
}

void IprController::SetEngineOverride(maxon::Int32 engine)
{
	const IprLocked held(_lock);
	_engineOverride = engine;
	_sceneDirty = true;
	_forceWhole = true;	// the settings go with a whole export, also in the manual mode
	if (!_viewStarted)
	{
		// Nothing is running: still remember the engine, so the pass list is the right one.
		cinema::BaseDocument* const doc = cinema::GetActiveDocument();
		const maxon::Int32 fromSettings = doc ? GetDocumentRenderSettings(doc).GetInt32(CYCLES_ENGINE, CYCLES_ENGINE_CYCLES) : CYCLES_ENGINE_CYCLES;
		const maxon::Int32 now = engine == NOTOK ? fromSettings : engine;
		if (now != _engine)
		{
			_engine = now;
			_passIndex = 0;
		}
	}
}

void IprController::SetResolutionScale(maxon::Float64 scale)
{
	const IprLocked held(_lock);
	_scale = maxon::ClampValue(scale, 0.05, 4.0);
	_sizeDirty = true;
	_windowSizeTime = 0.0;	// a choice from the menu takes effect at once
}

void IprController::SetFitWindow(maxon::Bool fit)
{
	const IprLocked held(_lock);
	_fitWindow = fit;
	_sizeDirty = true;
	_windowSizeTime = 0.0;
}

void IprController::SetWindowSize(maxon::Int32 width, maxon::Int32 height)
{
	const IprLocked held(_lock);
	if (width == _windowWidth && height == _windowHeight)
		return;
	_windowWidth = width;
	_windowHeight = height;
	_windowSizeTime = platform::UnixTime();
	_sizeDirty = true;
}

void IprController::RestartBlender()
{
	const IprLocked held(_lock);
	Stop();
	if (!_worker)
		return;
	iferr (IprTask* const close = NewObj(IprTask))
		return;
	close->kind = IprTask::KIND::CLOSE;
	_worker->Post(close);
}

void IprController::ShowBlenderWindow(maxon::Bool visible)
{
	const IprLocked held(_lock);
	_windowShown = visible;
	PostSimple("view_window", "visible", visible);
}

void IprController::KillBlenderForTest()
{
	const IprLocked held(_lock);
	if (_worker)
		_worker->KillBlenderForTest();
}

void IprController::MarkSceneChanged()
{
	const IprLocked held(_lock);
	_activityTime = platform::UnixTime();
	if (!_sceneDirty)
		_dirtySince = _activityTime;
	_sceneDirty = true;
}

void IprController::NoteRedraw()
{
	const IprLocked held(_lock);
	_activityTime = platform::UnixTime();
	if (!_redrawn && !_sceneDirty)
		_dirtySince = _activityTime;
	_redrawn = true;
}

void IprController::ForgetFollowed()
{
	_nodes.Reset();
	_objects.Reset();
	_ownedIds.Reset();
	_references.Reset();
	_nodeOf.Reset();
	_nodesValid = false;
	_anyDeformed = false;
	_recheck = false;
	_meshQuietUntil = 0.0;
	_counters = DocumentCounters();
	_redrawn = false;
	_settlePending = false;
	_wholeNeeded = false;
	_refreshWaiting = false;
	_retry = false;
}

void IprController::NoteFrame(const Frame& frame)
{
	const IprLocked held(_lock);
	// A few thousand pixels spread over the picture tell "same picture" from "changed".
	const maxon::Int valueCount = frame.pixels.GetCount();
	const maxon::Int step = maxon::Max(maxon::Int(4), (valueCount / 4096) & ~maxon::Int(3)) + 4;
	maxon::UInt64 checksum = maxon::UInt64(frame.width) * 0x9E3779B97F4A7C15ull + maxon::UInt64(frame.height);
	for (maxon::Int i = 0; i + 2 < valueCount; i += step)
	{
		maxon::UInt32 bits[3];
		maxon::MemCopyType(reinterpret_cast<maxon::Char*>(bits), reinterpret_cast<const maxon::Char*>(frame.pixels.GetFirst() + i), 12);
		checksum = (checksum ^ bits[0] ^ (maxon::UInt64(bits[1]) << 21) ^ (maxon::UInt64(bits[2]) << 42)) * 0x100000001B3ull;
	}
	_frameChecksum = checksum;
	if (_latencyArmed && checksum != _armedChecksum)
	{
		_latencyArmed = false;
		const maxon::Float64 now = platform::UnixTime();
		const maxon::Float64 answered = _worker ? maxon::ClampValue(_worker->GetLastSceneAnswerTime(), _postTime, now) : _postTime;
		_latencyMs = (now - _changeTime) * 1000.0;
		_latencyParts[0] = (_postTime - _changeTime) * 1000.0;
		_latencyParts[1] = (answered - _postTime) * 1000.0;
		const maxon::Float64 arrived = maxon::ClampValue(_frameArrival, answered, now);
		_latencyParts[2] = (arrived - answered) * 1000.0;
		_latencyParts[3] = (now - arrived) * 1000.0;
	}
}

// --- following the document -----------------------------------------------------------------

namespace
{
// Marks the controller as inside an export or a step for a scope (IprController::_exporting).
class Exporting
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(Exporting);

public:
	explicit Exporting(maxon::Bool& flag) : _flag(flag), _before(flag) { _flag = true; }
	~Exporting() { _flag = _before; }

private:
	maxon::Bool& _flag;
	maxon::Bool _before;
};
}

maxon::Result<void> IprController::SyncScene(maxon::Bool clear)
{
	iferr_scope;
	const Exporting exporting(_exporting);

	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "There is no document to render."_s);
	_document = doc;
	if (_resend)
	{
		clear = true;
		_resend = false;
	}
	if (clear)
	{
		_sceneState.Reset();
		_cameraState.Reset();
		_sentView = "?"_s;
		_sentRegion = "?"_s;
	}
	// What this export sees of the document's change counters: the gate and FollowStep() compare with it.
	const DocumentCounters counters = ReadCounters(doc);
	const maxon::UInt32 otherCounters = ReadOtherCounters(doc);

	// The Blender that renders: what its version lacks is left out of what is sent.
	const BlenderInstall blender = GetBlenderInUse();

	// Render settings, when they differ from what Blender has. The thread count goes with them, so it is
	// sent when the IPR starts and when the settings or the preference change, never during a drag:
	// changing it recreates Cycles' render session.
	cinema::BaseContainer data = GetDocumentRenderSettings(doc);
	LeaveThreadsFree(data);
	_askedDevice = data.GetInt32(CYCLES_DEVICE, CYCLES_DEVICE_CPU);
	{
		JsonWriter settings;
		maxon::BaseArray<maxon::String> warnings;
		const maxon::Int32 engine = WriteRenderSettings(settings, data, doc, _engineOverride, blender.Code(), warnings);
		for (const maxon::String& warning : warnings)
			ReportOnce(warning);
		if (settings.GetText() != _sentSettings)
		{
			_sentSettings = settings.GetText();
			_worker->PostRequest(settings);
			if (engine != _engine)
			{
				// Each engine has its own passes; begin with the beauty pass again.
				_engine = engine;
				_passIndex = 0;
				if (_viewStarted)
					PostPass();
			}
		}
	}

	SyncSize(doc);

	// The scene without its camera; SyncCamera() sends that.
	_exportSettings = GetExportSettings(data, _width, _height);
	_exportSettings.blenderVersion = blender.Code();
	_exportSettings.blenderVersionText = blender.versionText;
	_exportSettings.eevee = _engine == CYCLES_ENGINE_EEVEE;	// the IPR's engine menu may override the render settings
	_exportSettings.camera = ResolveCamera(doc);
	_exportSettings.writeCamera = false;
	// Meshes Blender holds, made from what is there now, are not exported again (Preferences >
	// Cycles Render > IPR, "Skip Unchanged Meshes"). A scene sent from scratch is exported whole.
	const maxon::Bool skip = GetIprSkipUnchangedMeshes();
	_exportSettings.reuse = !clear && skip ? &_sceneState : nullptr;
	_exportSettings.noteSources = skip;
	// What can be written alone in a step of a change in progress (FollowStep).
	_exportSettings.noteFollowed = GetIprLiveUpdates() && GetIprOptimizedUpdates();
	_exportSettings.hashBytes = IsIprUnoptimized();
	_exportedTime = doc->GetTime();
	_exportedTimeKnown = true;

	const maxon::Float64 exportStart = platform::UnixTime();
	SceneExporter exporter(doc, _exportSettings);
	// The exporter has its copy; the settings kept for the steps name no object that may be gone by then.
	_exportSettings.camera = nullptr;
	exporter.Collect() iferr_return;
	const maxon::Float64 collected = platform::UnixTime();

	IprTask* task = NewObj(IprTask) iferr_return;
	finally
	{
		DeleteObj(task);
	};
	task->kind = IprTask::KIND::SCENE;
	task->scene = NewObj(SceneWriter, clear) iferr_return;
	exporter.Write(*task->scene) iferr_return;
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);
	const maxon::Float64 written = platform::UnixTime();

	const maxon::Int changes = task->scene->KeepChanges(_sceneState) iferr_return;
	if (task->scene->IsMissingKeptItems())
	{
		// A kept mesh that Blender turned out not to hold (never seen): the request lacks it, so the
		// scene is sent again from scratch, exported whole.
		ApplicationOutput("Cycles Render IPR: an unchanged mesh was not where it was expected in Blender; the scene is sent again.");
		return SyncScene(true);
	}
	_lastChangeCount = maxon::Int32(changes);
	NoteLights(exporter) iferr_return;
	NoteFollowed(doc, exporter) iferr_return;
	_counters = counters;
	_otherCounters = otherCounters;
	_redrawn = false;
	_settlePending = false;
	_recheck = false;
	_refreshWaiting = false;
	_forceWhole = false;
	_retry = false;
	const maxon::Float64 compared = platform::UnixTime();
	_lastStats = exporter.GetStats();
	_lastExportMilliseconds = (compared - exportStart) * 1000.0;
	TraceIpr("collect_ms", (collected - exportStart) * 1000.0);
	TraceIpr("write_ms", (written - collected) * 1000.0);
	TraceIpr("compare_ms", (compared - written) * 1000.0);
	TraceIpr("mesh_ms", _lastStats.meshMilliseconds);
	TraceIpr("meshes_kept", maxon::Float64(_lastStats.meshesKept));
	TraceIpr("meshes_exported", maxon::Float64(_lastStats.meshesExported));
	TraceIpr("export_ms", _lastExportMilliseconds);
	if (changes > 0 || clear)
	{
		TraceIpr("posted_scene", maxon::Float64(changes));
		_worker->Post(task);
		task = nullptr;
		// The clock runs from the moment Cinema 4D reported the change.
		ArmLatency(_dirtySince);
	}
	_dirtySince = 0.0;

	SyncCamera(doc) iferr_return;
	return maxon::OK;
}

void IprController::SyncSize(cinema::BaseDocument* doc)
{
	_sizeDirty = false;

	// The camera's picture has the shape of the Render Settings resolution, with its pixel aspect:
	// the IPR renders square pixels, so a picture of pixels twice as wide as high is shown twice
	// as wide, as it is meant to be seen (and with "25 %" to "200 %" it gets that many pixels more
	// across).
	maxon::Int32 width = 1280;
	maxon::Int32 height = 720;
	maxon::Float64 pixelAspect = 1.0;
	if (const cinema::RenderData* const renderData = doc->GetActiveRenderData())
	{
		width = maxon::Max(maxon::Int32(1), renderData->GetDataInstanceRef().GetInt32(RDATA_XRES, width));
		height = maxon::Max(maxon::Int32(1), renderData->GetDataInstanceRef().GetInt32(RDATA_YRES, height));
		pixelAspect = GetPixelAspect(renderData->GetDataInstanceRef());
	}
	_frameAspect = maxon::Float64(width) * pixelAspect / maxon::Float64(height);

	const maxon::Bool fit = _fitWindow && _windowWidth > 0 && _windowHeight > 0;
	if (fit)
	{
		width = maxon::ClampValue(_windowWidth, MIN_SIDE, MAX_SIDE);
		height = maxon::ClampValue(_windowHeight, MIN_SIDE, MAX_SIDE);
	}
	else
	{
		width = maxon::ClampValue(maxon::Int32(maxon::Float64(width) * pixelAspect * _scale + 0.5), MIN_SIDE, MAX_SIDE);
		height = maxon::ClampValue(maxon::Int32(maxon::Float64(height) * _scale + 0.5), MIN_SIDE, MAX_SIDE);
	}
	_framing = fit ? CameraFraming::Fit(width, height, _frameAspect) : CameraFraming::Full(width, height);

	if (width != _width || height != _height)
	{
		_width = width;
		_height = height;
		if (_viewStarted && _worker)
		{
			JsonWriter resize;
			resize.BeginObject();
			resize.String("op", "view_size"_s);
			resize.Int("width", width);
			resize.Int("height", height);
			_worker->PostRequest(resize);
		}
	}
	SyncRegion(doc);
}

void IprController::SyncRegion(cinema::BaseDocument* doc)
{
	// Cinema 4D gives the region as borders in pixels of the Render Settings resolution. The
	// camera's picture may be only a part of the IPR image (Fit Window).
	_hasRegion = false;
	const cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
	if (renderData && _width > 0 && _height > 0)
	{
		const cinema::BaseContainer& data = renderData->GetDataInstanceRef();
		const maxon::Float64 xres = maxon::Max(1.0, maxon::Float64(data.GetInt32(RDATA_XRES, 1280)));
		const maxon::Float64 yres = maxon::Max(1.0, maxon::Float64(data.GetInt32(RDATA_YRES, 720)));
		if (data.GetBool(RDATA_RENDERREGION))
		{
			const maxon::Float64 left = maxon::Clamp01(data.GetInt32(RDATA_RENDERREGION_LEFT) / xres);
			const maxon::Float64 right = maxon::Clamp01(1.0 - data.GetInt32(RDATA_RENDERREGION_RIGHT) / xres);
			const maxon::Float64 bottom = maxon::Clamp01(data.GetInt32(RDATA_RENDERREGION_BOTTOM) / yres);
			const maxon::Float64 top = maxon::Clamp01(1.0 - data.GetInt32(RDATA_RENDERREGION_TOP) / yres);
			if (right > left && top > bottom)
			{
				const maxon::Float64 frameX = _framing.frameWidth / maxon::Float64(_width);
				const maxon::Float64 frameY = _framing.frameHeight / maxon::Float64(_height);
				_region[0] = (1.0 - frameX) * 0.5 + left * frameX;
				_region[1] = (1.0 - frameY) * 0.5 + bottom * frameY;
				_region[2] = (1.0 - frameX) * 0.5 + right * frameX;
				_region[3] = (1.0 - frameY) * 0.5 + top * frameY;
				_hasRegion = true;
			}
		}
	}

	if (!_worker)
		return;
	JsonWriter request;
	request.BeginObject();
	request.String("op", "view_region"_s);
	if (_hasRegion)
		request.Floats("region", _region, 4);
	if (request.GetText() != _sentRegion)
	{
		_sentRegion = request.GetText();
		_worker->PostRequest(request);
	}
}

maxon::Bool IprController::GetRegion(maxon::Int32& x0, maxon::Int32& y0, maxon::Int32& x1, maxon::Int32& y1) const
{
	if (!_hasRegion)
		return false;
	// Rounded as Blender rounds its border.
	x0 = maxon::Int32(_region[0] * _width + 0.5);
	x1 = maxon::Int32(_region[2] * _width + 0.5);
	y0 = _height - maxon::Int32(_region[3] * _height + 0.5);
	y1 = _height - maxon::Int32(_region[1] * _height + 0.5);
	return x1 > x0 && y1 > y0;
}

maxon::Result<void> IprController::SyncCamera(cinema::BaseDocument* doc)
{
	iferr_scope;
	const Exporting exporting(_exporting);

	const maxon::TimeValue started = maxon::TimeValue::GetTime();
	finally { TraceIpr("camera_ms", (maxon::TimeValue::GetTime() - started).GetMilliseconds()); };
	// A locked camera is a set of values that never changes (SetCameraLock); it is sent like
	// any other, which changes nothing in Blender unless the image changed its shape.
	CameraValues camera;
	ImagerChain imager;
	if (_cameraLocked && _hasLockedCamera)
	{
		camera = _lockedCamera;
		imager.CopyFrom(_lockedImager) iferr_return;
	}
	else
	{
		// (On the evaluating thread: only a camera of the document the main thread noted; else the camera
		// waits for the main thread's next call.)
		const cinema::BaseObject* const object = FollowedCamera(doc);
		if (!object)
			return maxon::OK;
		camera = CameraValues::Read(object);
		imager = ImagerChain::Read(object) iferr_return;
	}

	// The camera's effects (the Imager tab of its Cycles Camera tag) are an item of their own, sent
	// alone like the camera, whatever the camera itself is sent as: a changed value or opacity is one
	// small request, which Blender shows without starting the render over (the bridge sets it on the
	// compositor it has). Without effects, and with none sent before, nothing is made here.
	if (!imager.IsEmpty() || _cameraState.Holds(ImagerItemId()))
	{
		IprTask* effects = NewObj(IprTask) iferr_return;
		finally
		{
			DeleteObj(effects);
		};
		effects->kind = IprTask::KIND::SCENE;
		effects->scene = NewObj(SceneWriter, false) iferr_return;
		const maxon::Bool written = ExportImager(*effects->scene, imager, DocumentColors(doc), _exportSettings.blenderVersion) iferr_return;
		if (!written)
			effects->scene->RemoveItem(ImagerItemId());
		for (const maxon::String& warning : effects->scene->GetWarnings())
			ReportOnce(warning);
		const maxon::Int changes = effects->scene->KeepChanges(_cameraState, true) iferr_return;
		if (changes > 0)
		{
			TraceIpr("posted_imager");
			_worker->Post(effects);
			effects = nullptr;
			ArmLatency(0.0);
		}
	}

	// The quick way: place Blender's own view. Nothing in Blender's scene changes then.
	JsonWriter view;
	if (WriteFreeView(view, Coords::FromDocument(doc), camera, _exportSettings.depthOfField, _framing))
	{
		if (view.GetText() != _sentView)
		{
			_sentView = view.GetText();
			TraceIpr("posted_view");
			_worker->PostRequest(view);
			// A camera move is found by polling, not announced: the clock starts when it is found.
			ArmLatency(0.0);
		}
		return maxon::OK;
	}

	// This camera needs a real camera object (depth of field, film offset, a parallel or
	// panoramic lens).
	IprTask* task = NewObj(IprTask) iferr_return;
	finally
	{
		DeleteObj(task);
	};
	task->kind = IprTask::KIND::SCENE;
	task->scene = NewObj(SceneWriter, false) iferr_return;
	ExportCamera(*task->scene, Coords::FromDocument(doc), camera, _exportSettings.depthOfField,
		_exportSettings.eevee && _exportSettings.blenderVersion < EEVEE_PANORAMA_VERSION, _framing) iferr_return;
	// Said once per session, also when the camera itself did not change (another engine chosen).
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);

	const maxon::Int changes = task->scene->KeepChanges(_cameraState, true) iferr_return;
	if (changes > 0 || !_sentView.IsEmpty())
	{
		TraceIpr("posted_camera");
		_worker->Post(task);
		task = nullptr;
		if (!_sentView.IsEmpty())
		{
			_sentView = maxon::String();
			PostSimple("view_camera", "camera", true);
		}
		ArmLatency(0.0);
	}
	return maxon::OK;
}

void IprController::Tick()
{
	const IprLocked held(_lock);
	if (!_worker)
		return;
	// A call that arrives while this thread is in the middle of an export: Cinema 4D runs jobs of the
	// main thread's queue, the scene hook's among them, inside some of its own calls (seen: inside
	// StopAllThreads() or DrawViews() of a script's loop), and the lock lets the same thread in again. What
	// the export has half written is not for a second one to read: the next call follows instead.
	if (_exporting)
	{
		TraceIpr("reentered");
		return;
	}
	TraceIpr("tick");
	// The manual mode, read here for every thread that follows (the Preferences are the main thread's).
	// Switched off while changes wait: the whole export it held back comes now.
	const maxon::Bool manual = GetIprLiveUpdates() && GetIprOptimizedUpdates() && GetIprManualRefresh();
	if (_manual && !manual && _refreshWaiting)
	{
		_sceneDirty = true;
		_forceWhole = true;
	}
	_manual = manual;
	if (_deferredFailure.IsPopulated())
	{
		const maxon::String message = _deferredFailure;
		_deferredFailure = maxon::String();
		if (_state != IPRSTATE::STOPPED && _state != IPRSTATE::FAILED)
			Fail(message);
	}

	ReportWarnings();

	maxon::String failure;
	if (_worker->HasFailed(failure))
	{
		_worker->ClearFailure();
		// A Blender that goes away while nothing renders is simply started again next time.
		if (_state != IPRSTATE::STOPPED && _state != IPRSTATE::FAILED)
			Fail(failure);
	}

	// Follow the session: its frame block is named after its token.
	maxon::String token;
	maxon::Int processId = 0;
	if (_worker->GetSession(token, processId))
	{
		if (token != _readerToken)
		{
			_readerToken = token;
			_processId = processId;
			iferr (_reader.Open(token))
				ApplicationOutput("Cycles Render IPR: @", err);
			if (_watcher)
				_watcher->Watch(token);
		}
	}
	else if (!_readerToken.IsEmpty())
	{
		_readerToken = maxon::String();
		_processId = 0;
		_reader.Close();
		if (_watcher)
			_watcher->Watch(maxon::String());
	}

	FollowPriority();

	if (_state == IPRSTATE::STOPPED || _state == IPRSTATE::FAILED)
		return;

	const maxon::Bool idle = _worker->GetPendingCount() == 0;
	if (_state == IPRSTATE::STARTING)
	{
		if (!idle)
			return;
		_state = IPRSTATE::RUNNING;
	}
	if (_state == IPRSTATE::PAUSED)
		return;

	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc)
		return;

	if (doc != _document)
	{
		// Another document became active: Blender gets that one, from scratch.
		_sceneDirty = false;
		iferr (SyncScene(true))
			Fail(err.GetMessage());
		return;
	}

	// With the Preferences' live updates off nothing is sent while the timeline plays, neither the scene
	// nor an animated camera: the IPR shows the frame playback stopped at (and the camera does not run
	// ahead of the objects).
	if (!GetIprLiveUpdates() && cinema::CheckIsRunning(cinema::CHECKISRUNNING::ANIMATIONRUNNING))
	{
		TraceIpr("playback_held");
		return;
	}

	// Playback changes the document's time without any change event (measured: none in 3 s of
	// playing, an EVMSG_CHANGE only when it stopped), so the time is compared on every tick; the
	// window's timer runs while Cinema 4D plays. This also follows a time set without an event.
	if (_exportedTimeKnown && doc->GetTime() != _exportedTime)
	{
		MarkSceneChanged();
		// (The manual mode answers with a step, which exports no time: one step per time.)
		if (_manual)
			_exportedTime = doc->GetTime();
	}

	// While Blender is still busy with the last change, wait: the next export then covers
	// everything that happened in the meantime.
	if (!idle)
		return;

	if (_sceneDirty)
	{
		_sceneDirty = false;
		// A finished edit: the scene whole; in the manual mode what a step can send (as after a redraw).
		const maxon::Bool step = _manual && !_forceWhole && !IsIprUnoptimized();
		if (step)
		{
			_redrawn = false;
			iferr (FollowRedraw(doc))
				Fail(err.GetMessage());
		}
		else
		{
			iferr (SyncScene(false))
				Fail(err.GetMessage());
		}
	}
	else if (_redrawn)
	{
		_redrawn = false;
		iferr (FollowRedraw(doc))
			Fail(err.GetMessage());
	}
	else if (_settlePending && platform::UnixTime() - _activityTime >= SETTLE_SECONDS)
	{
		if (_manual)
		{
			// Things have stood still: no whole export in the manual mode. A step that was postponed is tried
			// again; without one nothing is looked at (a change no counter shows waits for Reload).
			_settlePending = false;
			const DocumentCounters counters = ReadCounters(doc);
			if (_retry || _recheck || !(counters == _counters))
			{
				TraceIpr("settle_step");
				_recheck = false;
				iferr (const ALONE alone = SyncAlone(doc, counters, true))
				{
					Fail(err.GetMessage());
					return;
				}
				if (alone == ALONE::NO)
				{
					iferr (SyncScene(false))
						Fail(err.GetMessage());
					return;
				}
				_settlePending = alone == ALONE::POSTPONED;
			}
			iferr (SyncView(doc))
				Fail(err.GetMessage());
			return;
		}
		// Things have stood still: Blender now gets exactly what a whole export gives.
		++_settleCount;
		TraceIpr("settle");
		iferr (SyncScene(false))
			Fail(err.GetMessage());
	}
	else
	{
		iferr (SyncView(doc))
			Fail(err.GetMessage());
	}
}

maxon::Result<void> IprController::SyncView(cinema::BaseDocument* doc)
{
	// The window got another size, or the menu asked for one: no need to look at the scene.
	if (_sizeDirty && platform::UnixTime() - _windowSizeTime >= WINDOW_SIZE_SETTLE_SECONDS)
		SyncSize(doc);
	return SyncCamera(doc);
}

void IprController::FollowPriority()
{
	if (_processId == 0)
	{
		_priorityProcess = 0;
		return;
	}
	const maxon::Float64 now = platform::UnixTime();
	if (_processId == _priorityProcess && now >= _priorityTime && now - _priorityTime < 1.0)
		return;
	_priorityTime = now;
	const maxon::Int32 own = platform::GetOwnPriorityRank();
	if (own < 0 || (_processId == _priorityProcess && own == _ownPriority))
		return;
	const maxon::Int32 wanted = BlenderPriorityRank(own);
	if (!platform::SetProcessPriorityRank(_processId, wanted))
		return;	// tried again in a second
	_priorityProcess = _processId;
	_ownPriority = own;
	_blenderPriority = wanted;
	TraceIpr("priority", maxon::Float64(own * 10 + wanted));
}

maxon::String IprController::GetDeviceText() const
{
	if (!_worker || _state == IPRSTATE::STOPPED || _state == IPRSTATE::FAILED)
		return maxon::String();
	const IprDeviceInfo info = _worker->GetDeviceInfo();
	if (!info.known)
		return maxon::String();
	// Blender names its graphics card as the driver does ("NVIDIA GeForce RTX 4090/PCIe/SSE2").
	maxon::String display = info.displayGpu;
	maxon::Int slash = 0;
	if (display.Find("/"_s, &slash))
		display = display.GetLeftPart(slash);
	if (info.engine == "BLENDER_EEVEE"_s)
		return display.IsEmpty() ? "Eevee"_s : "Eevee on "_s + display;
	if (info.device == "GPU"_s)
		return FormatString("Cycles on @: @", BackendLabel(info.backend), info.devices);
	// Why the CPU: the Render Settings ask for it, or no GPU could be used. (A scene whose Render Settings
	// said CPU while the Preferences named a GPU type otherwise looks like a slow GPU.)
	return FormatString("Cycles on CPU, @ threads @", info.threads, _askedDevice == CYCLES_DEVICE_CPU ? "(Render Settings)"_s : "(no GPU found)"_s);
}

maxon::Result<maxon::String> IprController::VerifySync()
{
	iferr_scope;
	const IprLocked held(_lock);
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc || doc != _document)
		return "{\"problem\":\"the IPR holds another document or none\"}"_s;
	ExportSettings settings = _exportSettings;
	settings.reuse = nullptr;
	settings.noteSources = false;
	settings.camera = ResolveCamera(doc);
	SceneExporter exporter(doc, settings);
	exporter.Collect() iferr_return;
	SceneWriter writer(false);
	exporter.Write(writer) iferr_return;
	SyncState copy;
	copy.CopyFrom(_sceneState) iferr_return;
	writer.KeepChanges(copy) iferr_return;
	const maxon::BaseArray<maxon::String> changed = writer.GetChangedIds() iferr_return;
	JsonWriter json;
	json.BeginObject();
	json.Int("items", writer.GetItemCount());
	json.BeginArray("differences");
	for (const maxon::String& id : changed)
		json.String(nullptr, id);
	json.EndArray();
	json.EndObject();
	return json.GetText();
}

void IprController::FollowCamera()
{
	const IprLocked held(_lock);
	if (!_worker || _state != IPRSTATE::RUNNING || _worker->GetPendingCount() != 0 || _deferredFailure.IsPopulated() || _exporting)
		return;
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc || doc != _document)
		return;
	iferr (SyncCamera(doc))
		Fail(err.GetMessage());
}

maxon::Result<void> IprController::NoteLights(SceneExporter& exporter)
{
	iferr_scope;
	_lights.Reset();
	// (The world of Cycles Environments follows the same way as a light: written again by every step.)
	ForgetWorld();
	const SceneExporter::WrittenWorld& world = exporter.GetWrittenWorld();
	if (world.alone)
	{
		for (maxon::Int role = 0; role < 3; ++role)
		{
			if (!world.objects[role])
				continue;
			_worldLinks[role] = cinema::BaseLink::Alloc();
			if (!_worldLinks[role])
				return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
			_worldLinks[role]->SetLink(const_cast<cinema::BaseObject*>(world.objects[role]));
		}
		// (Its objects are compared for being there only; the links find them again.)
		_writtenWorld = world;
		_worldAlone = true;
	}
	// The objects nodes name follow the same way: each one's `empty` is written again by every step.
	_empties.Reset();
	for (const NamedObject& named : exporter.GetNamedObjects())
	{
		FollowedEmpty followed;
		followed.link = cinema::BaseLink::Alloc();
		if (!followed.link)
			return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
		followed.link->SetLink(named.object);
		followed.id = named.id;
		_empties.Append(std::move(followed)) iferr_return;
	}
	maxon::BaseArray<WrittenLight> written = exporter.TakeWrittenLights();
	for (WrittenLight& light : written)
	{
		FollowedLight followed;
		followed.link = cinema::BaseLink::Alloc();
		if (!followed.link)
			return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
		followed.link->SetLink(light.object);
		followed.id = std::move(light.id);
		followed.linked = light.linked;
		followed.linking = std::move(light.linking);
		_lights.Append(std::move(followed)) iferr_return;
	}
	return maxon::OK;
}

void IprController::FollowLights()
{
	const IprLocked held(_lock);
	if (!_worker || _state != IPRSTATE::RUNNING || _worker->GetPendingCount() != 0 || _deferredFailure.IsPopulated() || _exporting)
		return;
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc || doc != _document)
		return;
	iferr (SyncLights(doc))
		Fail(err.GetMessage());
}

maxon::Result<void> IprController::SyncLights(cinema::BaseDocument* doc)
{
	iferr_scope;
	if (_lights.IsEmpty())
		return maxon::OK;
	const Exporting exporting(_exporting);

	// A clock finer than UnixTime(), whose steps of about a millisecond would read 0 here.
	const maxon::TimeValue start = maxon::TimeValue::GetTime();
	IprTask* task = NewObj(IprTask) iferr_return;
	finally
	{
		DeleteObj(task);
	};
	task->kind = IprTask::KIND::SCENE;
	task->scene = NewObj(SceneWriter, false) iferr_return;
	maxon::Bool lost = false;
	for (const FollowedLight& light : _lights)
	{
		const cinema::BaseObject* const object = static_cast<const cinema::BaseObject*>(light.link->GetLink(doc, Obase));
		if (!object)
		{
			// Deleted (an undo, say): the full export removes it.
			lost = true;
			continue;
		}
		const maxon::Bool written = SceneExporter::WriteLightAlone(*task->scene, doc, _exportSettings, object, light.id, light.linked ? &light.linking : nullptr) iferr_return;
		// A light that emits nothing now (its intensity dragged to 0) is taken out of Blender's scene,
		// as the full export does.
		if (!written)
			task->scene->RemoveItem(light.id);
	}
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);
	const maxon::Int changes = task->scene->KeepChanges(_sceneState, true) iferr_return;
	_lastLightsMilliseconds = (maxon::TimeValue::GetTime() - start).GetMilliseconds();
	TraceIpr("lights_ms", _lastLightsMilliseconds);
	_lastChangeCount = maxon::Int32(changes);
	if (changes > 0)
	{
		TraceIpr("posted_lights", maxon::Float64(changes));
		_worker->Post(task);
		task = nullptr;
		ArmLatency(0.0);
	}
	if (lost)
		MarkSceneChanged();
	return maxon::OK;
}

// --- a change in progress: the gate, and what moves sent alone ---------------------------------

IprController::DocumentCounters IprController::ReadCounters(cinema::BaseDocument* doc)
{
	DocumentCounters counters;
	counters.all = doc->GetHDirty(cinema::HDIRTYFLAGS::ALL);
	counters.time = doc->GetTime();
	counters.known = true;
	return counters;
}

// Not the node bits either: in Cinema 4D 2027 that counter moves whenever a generator with
// an input builds its cache anew (measured: a Cloner's child, its count, an effector, the spline of an
// Extrude; not in 2024), since the bits include the mark "used by a generator" that is set again then.
// What the exporter reads of them is that mark, which FollowStep() compares object by object. (2027 also
// counts the document's unit and its Linear Workflow switch there, which a step therefore sees as little
// as Cinema 4D 2024 does: they arrive with the whole export.)
maxon::UInt32 IprController::ReadOtherCounters(cinema::BaseDocument* doc)
{
	return doc->GetHDirty(cinema::HDIRTYFLAGS::ALL & ~(cinema::HDIRTYFLAGS::OBJECT | cinema::HDIRTYFLAGS::OBJECT_MATRIX | cinema::HDIRTYFLAGS::TAG | cinema::HDIRTYFLAGS::NBITS));
}

// The counters of an object's tags (their number and order count too).
static maxon::UInt32 TagsDirty(const cinema::BaseObject* op)
{
	maxon::UInt32 dirty = 0;
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
		dirty = dirty * 31 + tag->GetDirty(cinema::DIRTYFLAGS::DATA) + 1;
	return dirty;
}

static maxon::UInt32 NameHash(const cinema::BaseObject* op)
{
	return maxon::UInt32(op->GetName().GetHashCode());
}

// The counters of everything an object generated: its cache, the objects in it, their caches and their
// deformed copies. For objects that show a deformed mesh: when a deformer moves, Cinema 4D 2024 counts
// that only on the object inside that carries the deformed copy, not on the generator around it.
static void MixCacheTree(const cinema::BaseObject* first, maxon::UInt32& sum);

static void MixCacheCounters(const cinema::BaseObject* op, maxon::UInt32& sum)
{
	sum = sum * 31 + op->GetDirty(cinema::DIRTYFLAGS::CACHE);
	if (const cinema::BaseObject* const deformed = op->GetDeformCache())
		sum = (sum * 31 + maxon::UInt32(reinterpret_cast<maxon::UInt>(deformed) >> 4)) * 31 + deformed->GetDirty(cinema::DIRTYFLAGS::DATA);
	MixCacheTree(op->GetCache(), sum);
}

static void MixCacheTree(const cinema::BaseObject* first, maxon::UInt32& sum)
{
	for (const cinema::BaseObject* op = first; op; op = op->GetNext())
	{
		sum = sum * 31 + op->GetDirty(cinema::DIRTYFLAGS::DATA);
		MixCacheCounters(op, sum);
		MixCacheTree(op->GetDown(), sum);
	}
}

// What a step compares of an object's cache: its cache counter, which moves when the cache is built
// anew, or with @p deformed everything in the cache (which reads the cache: only where that is safe).
static maxon::UInt32 CacheDirty(const cinema::BaseObject* op, maxon::Bool deformed)
{
	if (!deformed)
		return op->GetDirty(cinema::DIRTYFLAGS::CACHE);
	maxon::UInt32 sum = 0;
	MixCacheCounters(op, sum);
	return sum;
}

// The next object of a walk from the top, each object before the objects below it; @p depth follows.
static const cinema::BaseObject* NextInWalk(const cinema::BaseObject* op, maxon::Int& depth)
{
	if (op->GetDown())
	{
		++depth;
		return op->GetDown();
	}
	while (op && !op->GetNext())
	{
		op = op->GetUp();
		--depth;
	}
	return op ? op->GetNext() : nullptr;
}

maxon::Result<void> IprController::NoteFollowed(cinema::BaseDocument* doc, SceneExporter& exporter)
{
	iferr_scope;
	_nodes.Reset();
	_objects.Reset();
	_ownedIds.Reset();
	_references.Reset();
	_nodeOf.Reset();
	_nodesValid = false;
	_anyDeformed = false;
	if (!_exportSettings.noteFollowed)
		return maxon::OK;

	// The objects of the document, in the walk's order, with their counters as this export saw them.
	maxon::BaseArray<maxon::Int32> open;	// the nodes whose objects below are being listed
	maxon::Int depth = 0;
	for (const cinema::BaseObject* op = doc->GetFirstObject(); op; op = NextInWalk(op, depth))
	{
		while (open.GetCount() > depth)
		{
			_nodes[open[open.GetCount() - 1]].end = maxon::Int32(_nodes.GetCount());
			open.Pop();
		}
		FollowedNode node;
		node.object = op;
		node.matrixDirty = op->GetDirty(cinema::DIRTYFLAGS::MATRIX);
		node.dataDirty = op->GetDirty(cinema::DIRTYFLAGS::DATA);
		node.tagsDirty = TagsDirty(op);
		node.cacheDirty = op->GetDirty(cinema::DIRTYFLAGS::CACHE);
		node.nameHash = NameHash(op);
		node.renderMode = op->GetRenderMode();
		const maxon::Int32 type = op->GetType();
		// A light's values are written with the light alone, a camera's are sent by SyncCamera().
		// (A Cycles Environment's world is written by the step too; one that is not used is noted as fixed.)
		node.cameraOrLight = type == Olight || type == Orslight || type == ID_LIGHT_OBJECT || type == ID_ENVIRONMENT_OBJECT || type == Orscamera || op->IsInstanceOf(Ocamera);
		node.modifier = (op->GetInfo() & OBJECT_MODIFIER) != 0;
		node.consumed = op->GetBit(BIT_CONTROLOBJECT);
		_nodeOf.Insert(op, maxon::Int32(_nodes.GetCount())) iferr_return;
		open.Append(maxon::Int32(_nodes.GetCount())) iferr_return;
		_nodes.Append(node) iferr_return;
	}
	for (const maxon::Int32 index : open)
		_nodes[index].end = maxon::Int32(_nodes.GetCount());

	TakeNotes(exporter, nullptr) iferr_return;
	_materialIds = exporter.TakeMaterialIds();
	_anyLinking = exporter.HasLightLinking();
	_nodesValid = true;
	return maxon::OK;
}

maxon::Result<void> IprController::TakeNotes(SceneExporter& exporter, const maxon::BaseArray<maxon::UChar>* collected)
{
	iferr_scope;
	const auto taken = [collected](maxon::Int index) { return !collected || ((*collected)[index] & 1) != 0; };
	const maxon::Int count = _nodes.GetCount();

	// What each of these objects shows, as this export found it.
	for (maxon::Int index = 0; index < count; ++index)
	{
		if (!taken(index))
			continue;
		FollowedNode& node = _nodes[index];
		node.unfollowed = false;
		node.deformed = false;
		node.fixed = false;
		node.dataAlone = node.cameraOrLight;
	}
	for (const cinema::BaseObject* const owner : exporter.GetUnfollowedOwners())
	{
		if (const auto* const found = _nodeOf.Find(owner))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			node.unfollowed = true;
			node.dataAlone = false;	// a dome light is the world, a light of a generator is not written alone
		}
	}
	for (const cinema::BaseObject* const owner : exporter.GetDeformedOwners())
	{
		if (const auto* const found = _nodeOf.Find(owner))
			_nodes[found->GetValue()].deformed = true;
	}
	for (const cinema::BaseObject* const owner : exporter.GetFixedOwners())
	{
		if (const auto* const found = _nodeOf.Find(owner))
			_nodes[found->GetValue()].fixed = true;
	}
	_anyDeformed = false;
	for (maxon::Int index = 0; index < count; ++index)
	{
		FollowedNode& node = _nodes[index];
		// (An object that shows a deformed mesh is compared by the counters inside its cache.)
		if (taken(index))
			node.cacheDirty = CacheDirty(node.object, node.deformed);
		_anyDeformed = _anyDeformed || node.deformed;
	}

	// Which object shows which: those of the objects taken are replaced.
	maxon::BaseArray<FollowedReference> references;
	for (const FollowedReference& reference : _references)
	{
		if (!taken(reference.referrer))
		{
			references.Append(reference) iferr_return;
		}
	}
	for (const ObjectReference& reference : exporter.GetReferences())
	{
		const auto* const referrer = _nodeOf.Find(reference.referrer);
		const auto* const root = _nodeOf.Find(reference.root);
		if (referrer && root)
		{
			references.Append(FollowedReference{ referrer->GetValue(), root->GetValue() }) iferr_return;
		}
	}
	_references = std::move(references);
	for (FollowedNode& node : _nodes)
		node.referenced = false;
	for (const FollowedReference& reference : _references)
	{
		for (maxon::Int32 index = reference.root + 1; index < _nodes[reference.root].end; ++index)
			_nodes[index].referenced = true;
	}

	// The items, grouped by their owner's node: those of the objects taken are replaced, the others stay.
	maxon::BaseArray<WrittenObject> written = exporter.TakeWrittenObjects();
	maxon::BaseArray<OwnedItem> owned = exporter.TakeOwnedItems();
	maxon::BaseArray<maxon::Int32> objectCounts;
	maxon::BaseArray<maxon::Int32> ownedCounts;
	objectCounts.Resize(count) iferr_return;
	ownedCounts.Resize(count) iferr_return;
	for (maxon::Int index = 0; index < count; ++index)
	{
		objectCounts[index] = taken(index) ? 0 : _nodes[index].objectCount;
		ownedCounts[index] = taken(index) ? 0 : _nodes[index].ownedCount;
	}
	for (const WrittenObject& object : written)
	{
		const auto* const found = _nodeOf.Find(object.owner);
		if (found && taken(found->GetValue()))
			++objectCounts[found->GetValue()];
	}
	for (const OwnedItem& item : owned)
	{
		const auto* const found = _nodeOf.Find(item.owner);
		if (found && taken(found->GetValue()))
			++ownedCounts[found->GetValue()];
	}
	maxon::Int32 objectTotal = 0;
	maxon::Int32 ownedTotal = 0;
	for (maxon::Int index = 0; index < count; ++index)
	{
		objectTotal += objectCounts[index];
		ownedTotal += ownedCounts[index];
	}
	maxon::BaseArray<WrittenObject> objects;
	maxon::BaseArray<maxon::String> ownedIds;
	objects.Resize(objectTotal) iferr_return;
	ownedIds.Resize(ownedTotal) iferr_return;
	maxon::Int32 firstObject = 0;
	maxon::Int32 firstOwned = 0;
	for (maxon::Int index = 0; index < count; ++index)
	{
		FollowedNode& node = _nodes[index];
		if (!taken(index))
		{
			for (maxon::Int32 i = 0; i < node.objectCount; ++i)
				objects[firstObject + i] = std::move(_objects[node.firstObject + i]);
			for (maxon::Int32 i = 0; i < node.ownedCount; ++i)
				ownedIds[firstOwned + i] = std::move(_ownedIds[node.firstOwned + i]);
		}
		else
		{
			// Filled below, counting up again.
			node.objectCount = 0;
			node.ownedCount = 0;
		}
		node.firstObject = firstObject;
		node.firstOwned = firstOwned;
		firstObject += objectCounts[index];
		firstOwned += ownedCounts[index];
	}
	for (WrittenObject& object : written)
	{
		const auto* const found = _nodeOf.Find(object.owner);
		if (found && taken(found->GetValue()))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			objects[node.firstObject + node.objectCount++] = std::move(object);
		}
	}
	for (OwnedItem& item : owned)
	{
		const auto* const found = _nodeOf.Find(item.owner);
		if (found && taken(found->GetValue()))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			ownedIds[node.firstOwned + node.ownedCount++] = std::move(item.id);
		}
	}
	_objects = std::move(objects);
	_ownedIds = std::move(ownedIds);
	return maxon::OK;
}

void IprController::SetMeshQuietForTest(maxon::Float64 seconds)
{
	const IprLocked held(_lock);
	_meshQuietUntil = seconds > 0.0 ? platform::UnixTime() + seconds : 0.0;
}

maxon::Float64 IprController::GetMeshQuietMilliseconds() const
{
	const IprLocked held(_lock);
	return maxon::Max(0.0, _meshQuietUntil - platform::UnixTime()) * 1000.0;
}

IprController::ALONE IprController::Whole(WHOLE why)
{
	_lastWhole = why;
	TraceIpr("alone_refused", maxon::Float64(maxon::Int(why)));
	return ALONE::NO;
}

void IprController::Waits(WHOLE why)
{
	_lastWhole = why;
	_refreshWaiting = true;
	TraceIpr("alone_waits", maxon::Float64(maxon::Int(why)));
}

IprController::ALONE IprController::Refuse(WHOLE why)
{
	if (!_manual)
		return Whole(why);
	Waits(why);
	return ALONE::WAITS;
}

maxon::Result<IprController::ALONE> IprController::SyncAlone(cinema::BaseDocument* doc, const DocumentCounters& counters, maxon::Bool cachesReadable)
{
	iferr_scope;
	const Exporting exporting(_exporting);
	const maxon::TimeValue start = maxon::TimeValue::GetTime();
	_lastWhole = WHOLE::NONE;
	_lastAloneObjects = 0;
	_lastAloneOwners = 0;
	_lastAloneMeshes = 0;
	_lastAloneKept = 0;
	// Another time moves whatever is animated; anything but objects and tags (materials, render settings,
	// the hierarchy, layers, takes) is not looked at here.
	// (Nothing to compare with: a whole export, in the manual mode too.)
	if (!_nodesValid || !_counters.known)
		return Whole(WHOLE::DOCUMENT);
	const maxon::Bool manual = _manual;
	_retry = false;
	if (counters.time != _counters.time || ReadOtherCounters(doc) != _otherCounters)
	{
		if (!manual)
			return Whole(WHOLE::DOCUMENT);
		// The manual mode: what the objects show at this time, or beside that change, still goes.
		Waits(WHOLE::DOCUMENT);
	}
	// The counters of an object that shows a deformed mesh are inside its cache, which must not be read
	// while another thread may be building it.
	if (!cachesReadable && _anyDeformed)
	{
		++_postponedCount;
		TraceIpr("postponed", 1.0);
		_settlePending = true;
		_retry = true;
		return ALONE::POSTPONED;
	}

	IprTask* task = NewObj(IprTask) iferr_return;
	finally
	{
		DeleteObj(task);
	};
	task->kind = IprTask::KIND::SCENE;
	task->scene = NewObj(SceneWriter, false) iferr_return;
	const Coords coords = Coords::FromDocument(doc);

	// Every object of the document against what the last whole export saw of it. `moving` holds, per
	// level of the walk, whether the object there or one above it moved, `matrices` its matrix then, and
	// `chain` its node. `marks` says per node what the step does with it: COLLECT, collected and written
	// again; SEED, it moved below an object an instance shows (the instance is collected again).
	static const maxon::UChar COLLECT = 1;
	static const maxon::UChar SEED = 2;
	struct Seen
	{
		maxon::Int32 node;
		maxon::UInt32 matrixDirty;
		maxon::UInt32 dataDirty;
		maxon::UInt32 tagsDirty;
		maxon::UInt32 cacheDirty;
		maxon::Bool consumed;
	};
	struct Moved
	{
		maxon::Int32 node;
		cinema::Matrix mg;
	};
	maxon::BaseArray<Seen> seen;
	maxon::BaseArray<Moved> moved;
	maxon::BaseArray<maxon::Bool> moving;
	maxon::BaseArray<cinema::Matrix> matrices;
	maxon::BaseArray<maxon::Int32> chain;
	maxon::BaseArray<maxon::UChar> marks;
	maxon::Bool collecting = false;
	maxon::Bool seeded = false;
	const auto mark = [&](maxon::Int32 first, maxon::Int32 end, maxon::UChar what) -> maxon::Result<void>
	{
		iferr_scope;
		if (marks.IsEmpty())
		{
			marks.Resize(_nodes.GetCount()) iferr_return;
			for (maxon::UChar& value : marks)
				value = 0;
		}
		for (maxon::Int32 i = first; i < end; ++i)
			marks[i] |= what;
		collecting = collecting || what == COLLECT;
		seeded = seeded || what == SEED;
		return maxon::OK;
	};
	maxon::Int depth = 0;
	maxon::Int32 index = 0;
	for (const cinema::BaseObject* op = doc->GetFirstObject(); op; op = NextInWalk(op, depth), ++index)
	{
		// An object added, removed or put elsewhere: the hierarchy counter said so already; a guard.
		if (index >= _nodes.GetCount() || _nodes[index].object != op)
			return Refuse(WHOLE::HIERARCHY);
		const FollowedNode& node = _nodes[index];
		const maxon::UInt32 matrixDirty = op->GetDirty(cinema::DIRTYFLAGS::MATRIX);
		const maxon::UInt32 dataDirty = op->GetDirty(cinema::DIRTYFLAGS::DATA);
		const maxon::UInt32 tagsDirty = TagsDirty(op);
		const maxon::UInt32 cacheDirty = CacheDirty(op, node.deformed);
		// The render dot (which counts as a change of the matrix): the objects below inherit it.
		if (op->GetRenderMode() != node.renderMode)
		{
			if (!manual)
				return Whole(WHOLE::RENDER_DOT);
			Waits(WHOLE::RENDER_DOT);
		}
		// A tag: its material, its selection, what it hands to the objects below (not looked into). Not so
		// the tags of a deformer with nothing below it: its points are tag data (the cage of an FFD), and
		// they change what the objects around it show, like its values.
		const maxon::Bool tagsChanged = tagsDirty != node.tagsDirty;
		if (tagsChanged && !node.dataAlone && !(node.modifier && !op->GetDown()))
		{
			if (!manual)
				return Whole(WHOLE::TAG);
			Waits(WHOLE::TAG);
		}
		// The mark "used by a generator" (as its input, or as a generator that has a cache): whether the
		// object shows something itself. Measured: it can be off for one evaluation and on again at the next
		// (a spline below an Extrude when its rounding was switched on), and it goes off with a generator's
		// green check.
		const maxon::Bool consumed = op->GetBit(BIT_CONTROLOBJECT);
		const maxon::Bool dataChanged = dataDirty != node.dataDirty || consumed != node.consumed;
		const maxon::Bool cacheChanged = cacheDirty != node.cacheDirty;
		const maxon::Bool movedItself = matrixDirty != node.matrixDirty;
		if (chain.GetCount() <= depth)
		{
			chain.Resize(depth + 1) iferr_return;
			moving.Resize(depth + 1) iferr_return;
			matrices.Resize(depth + 1) iferr_return;
		}
		chain[depth] = index;
		// Its own data (a generator's parameter) or a cache built anew (its input changed, it is deformed
		// anew, something it reads changed): what it shows is collected again.
		if ((dataChanged && !node.dataAlone) || cacheChanged)
		{
			// Its name is part of what the objects below it are sent with (the keys of Redshift's random nodes).
			if (dataChanged && NameHash(op) != node.nameHash)
			{
				if (!manual)
					return Whole(WHOLE::NAME);
				Waits(WHOLE::NAME);
			}
			mark(index, index + 1, COLLECT) iferr_return;
		}
		// A deformer changes what the object above it and everything below that show.
		if (node.modifier && (dataChanged || movedItself || tagsChanged) && depth > 0)
		{
			mark(chain[depth - 1], _nodes[chain[depth - 1]].end, COLLECT) iferr_return;
		}
		const maxon::Bool above = depth > 0 && moving[depth - 1];
		const maxon::Bool moves = movedItself || above;
		moving[depth] = moves;
		if (moves)
		{
			// (Its place as the exporter's walk forms it: that function's product, continued here.)
			matrices[depth] = above ? matrices[depth - 1] * op->GetMl() : SceneExporter::WalkMatrix(op);
			if (node.unfollowed)
			{
				// What it shows has no item of its own place (a Cloner's clones, an instance): collected again.
				mark(index, index + 1, COLLECT) iferr_return;
			}
			else if (node.objectCount > 0)
			{
				moved.Append(Moved{ index, matrices[depth] }) iferr_return;
			}
			// Below an object an instance shows: the instance shows the move too.
			if (movedItself && node.referenced)
			{
				mark(index, index + 1, SEED) iferr_return;
			}
		}
		if (movedItself || dataChanged || cacheChanged || tagsChanged)
		{
			seen.Append(Seen{ index, matrixDirty, dataDirty, tagsDirty, cacheDirty, consumed }) iferr_return;
		}
	}
	if (index != _nodes.GetCount())
		return Refuse(WHOLE::HIERARCHY);

	// What shows the same object goes together: an object that changed below one an instance shows takes
	// the instance with it, and an instance takes what it shows (the first of them in the document names
	// their mesh, so a whole export's items come out only when all of them are written).
	if ((collecting || seeded) && !_references.IsEmpty())
	{
		for (maxon::Bool grown = true; grown;)
		{
			grown = false;
			for (const FollowedReference& reference : _references)
			{
				const maxon::Int32 end = _nodes[reference.root].end;
				if ((marks[reference.referrer] & COLLECT) == 0)
				{
					maxon::Bool touched = false;
					for (maxon::Int32 i = reference.root; i < end && !touched; ++i)
						touched = marks[i] != 0;
					if (!touched)
						continue;
					marks[reference.referrer] |= COLLECT;
					collecting = true;
					grown = true;
				}
				for (maxon::Int32 i = reference.root; i < end; ++i)
				{
					if ((marks[i] & COLLECT) == 0)
					{
						marks[i] |= COLLECT;
						grown = true;
					}
				}
			}
		}
	}

	maxon::BaseArray<const cinema::BaseObject*> owners;
	if (collecting)
	{
		for (maxon::Int32 i = 0; i < _nodes.GetCount(); ++i)
		{
			if ((marks[i] & COLLECT) == 0)
				continue;
			// A volume, hair, the sky, a light of a generator, a mesh shared by content: the whole export's.
			if (_nodes[i].fixed)
			{
				if (!manual)
					return Whole(WHOLE::FIXED);
				// The manual mode: nothing is collected in this step (what shows the same object goes
				// together, so no part of the marked objects is left out alone); what only moved still goes.
				Waits(WHOLE::FIXED);
				owners.Reset();
				collecting = false;
				for (maxon::UChar& value : marks)
					value &= maxon::UChar(~COLLECT);
				break;
			}
			owners.Append(_nodes[i].object) iferr_return;
		}
	}
	if (collecting)
	{
		// Collecting reads caches and exports meshes: not while another thread may be evaluating the document,
		// and not again right after a step that took long (the newest state goes with a later step, or with
		// the whole export at the end).
		if (!cachesReadable || platform::UnixTime() < _meshQuietUntil)
		{
			++_postponedCount;
			TraceIpr("postponed", cachesReadable ? 0.0 : 1.0);
			_settlePending = true;
			_retry = true;
			return ALONE::POSTPONED;
		}
	}

	// The objects that only moved, each item as the whole export wrote it at another place.
	for (const Moved& entry : moved)
	{
		if (collecting && (marks[entry.node] & COLLECT) != 0)
			continue;
		const FollowedNode& node = _nodes[entry.node];
		for (maxon::Int32 i = 0; i < node.objectCount; ++i)
			SceneExporter::WriteObjectAlone(*task->scene, coords, _objects[node.firstObject + i], entry.mg);
		_lastAloneObjects += node.objectCount;
	}

	// What the collected objects show, as a whole export would write it for them.
	ExportSettings settings = _exportSettings;
	settings.noteFollowed = true;
	settings.materials = &_materialIds;
	// Meshes whose content did not change are kept as Blender holds them, as in a whole export (also after
	// the first one, which was sent from scratch and so had nothing to keep).
	settings.reuse = _exportSettings.noteSources ? &_sceneState : nullptr;
	SceneExporter exporter(doc, settings);
	if (collecting)
	{
		exporter.CollectOwners(owners) iferr_return;
		exporter.Write(*task->scene) iferr_return;
		if (exporter.IsRefused())
			return Refuse(WHOLE::REFUSED);
		_lastAloneOwners = maxon::Int32(owners.GetCount());
		_lastAloneMeshes = maxon::Int32(exporter.GetStats().meshesExported);
		_lastAloneKept = maxon::Int32(exporter.GetStats().meshesKept);

		// Items these objects had and have no longer go. When items come or go, the lights' include and
		// exclude lists, which name items, would have to follow: left to the whole export.
		maxon::HashSet<maxon::String> now;
		for (const OwnedItem& item : exporter.GetOwnedItems())
		{
			now.Insert(item.id) iferr_return;
		}
		maxon::Bool differs = false;
		maxon::Int before = 0;
		for (maxon::Int32 i = 0; i < _nodes.GetCount(); ++i)
		{
			if ((marks[i] & COLLECT) == 0)
				continue;
			const FollowedNode& node = _nodes[i];
			before += node.ownedCount;
			for (maxon::Int32 k = 0; k < node.ownedCount; ++k)
			{
				const maxon::String& id = _ownedIds[node.firstOwned + k];
				if (!now.Contains(id))
				{
					task->scene->RemoveItem(id);
					differs = true;
				}
			}
		}
		if ((differs || exporter.GetOwnedItems().GetCount() != before) && _anyLinking)
			return Refuse(WHOLE::LINKING);
	}

	// The lights, each written alone (SceneExporter::WriteLightAlone): their values and their places.
	maxon::Bool lost = false;
	for (const FollowedLight& light : _lights)
	{
		const cinema::BaseObject* const object = static_cast<const cinema::BaseObject*>(light.link->GetLink(doc, Obase));
		if (!object)
		{
			lost = true;
			continue;
		}
		const maxon::Bool written = SceneExporter::WriteLightAlone(*task->scene, doc, _exportSettings, object, light.id, light.linked ? &light.linking : nullptr) iferr_return;
		if (!written)
			task->scene->RemoveItem(light.id);
	}
	if (lost)
	{
		if (!manual)
			return Whole(WHOLE::LIGHT_GONE);
		Waits(WHOLE::LIGHT_GONE);
	}
	// The world of Cycles Environments, written alone: their values, their places and a linked light's sun
	// (SceneExporter::WriteWorldAlone).
	if (_worldAlone)
	{
		const cinema::BaseObject* environments[3] = { nullptr, nullptr, nullptr };
		for (maxon::Int role = 0; role < 3; ++role)
			environments[role] = _worldLinks[role] ? static_cast<const cinema::BaseObject*>(_worldLinks[role]->GetLink(doc, Obase)) : nullptr;
		const maxon::Bool written = SceneExporter::WriteWorldAlone(*task->scene, doc, _exportSettings, environments, _writtenWorld) iferr_return;
		if (!written)
		{
			if (!manual)
				return Whole(WHOLE::ENVIRONMENT);
			Waits(WHOLE::ENVIRONMENT);
		}
	}
	// The objects nodes of the materials name: each one's `empty` at its place (SceneExporter::WriteEmptyAlone), whatever
	// moved it (itself, an object above it) and whatever it is (a null, a camera, an object that renders nothing).
	// Unchanged ones are dropped below. One that is gone changes what its material's node gives: the whole export's.
	maxon::Bool namedLost = false;
	for (const FollowedEmpty& named : _empties)
	{
		const cinema::BaseObject* const object = static_cast<const cinema::BaseObject*>(named.link->GetLink(doc, Obase));
		if (!object)
		{
			namedLost = true;
			continue;
		}
		SceneExporter::WriteEmptyAlone(*task->scene, coords, named.id, object);
	}
	if (namedLost)
	{
		if (!manual)
			return Whole(WHOLE::NAMED_GONE);
		Waits(WHOLE::NAMED_GONE);
	}
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);

	const maxon::Int changes = task->scene->KeepChanges(_sceneState, true) iferr_return;
	if (task->scene->IsMissingKeptItems())
	{
		// A kept mesh that Blender turned out not to hold (never seen): the scene is sent again from scratch.
		_resend = true;
		return Refuse(WHOLE::MESH_NOT_HELD);
	}
	for (const Seen& entry : seen)
	{
		FollowedNode& node = _nodes[entry.node];
		node.matrixDirty = entry.matrixDirty;
		node.dataDirty = entry.dataDirty;
		node.tagsDirty = entry.tagsDirty;
		node.cacheDirty = entry.cacheDirty;
		node.consumed = entry.consumed;
	}
	if (collecting)
	{
		TakeNotes(exporter, &marks) iferr_return;
	}
	_counters = counters;
	_settlePending = true;
	_lastChangeCount = maxon::Int32(changes);
	_lastAloneMilliseconds = (maxon::TimeValue::GetTime() - start).GetMilliseconds();
	TraceIpr("alone_ms", _lastAloneMilliseconds);
	if (collecting)
	{
		TraceIpr("alone_meshes", maxon::Float64(_lastAloneMeshes));
		// The guard: a step that collected and took longer than MESH_STEP_FREE_MILLISECONDS is followed by
		// MESH_STEP_QUIET_FACTOR times its length without one.
		_meshQuietUntil = _lastAloneMilliseconds > MESH_STEP_FREE_MILLISECONDS ? platform::UnixTime() + _lastAloneMilliseconds * MESH_STEP_QUIET_FACTOR / 1000.0 : 0.0;
	}
	if (changes > 0)
	{
		++_aloneCount;
		TraceIpr("posted_alone", maxon::Float64(changes));
		_worker->Post(task);
		task = nullptr;
		ArmLatency(_dirtySince);
	}
	_dirtySince = 0.0;
	return ALONE::YES;
}

void IprController::FollowStep(maxon::Bool allowWhole, maxon::Bool cachesReadable)
{
	const IprLocked held(_lock);
	// (After a step that failed on the evaluating thread nothing is sent until Tick() has taken the
	// failure over; in a modal loop that is when the loop has ended. And nothing from inside an export:
	// Tick().)
	if (!_worker || _state != IPRSTATE::RUNNING || _deferredFailure.IsPopulated() || _exporting)
		return;
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc || doc != _document)
		return;
	FollowDocument(doc, allowWhole, cachesReadable);
}

maxon::Bool IprController::FollowEvaluated(cinema::BaseDocument* doc, maxon::Bool optimized)
{
	// Never waiting: the main thread may hold the lock while it waits for this thread to end.
	if (!_lock.TryLock())
	{
		// (Which document the IPR shows cannot be read without the lock: the window is asked to look.)
		TraceIpr("hook_busy");
		return true;
	}
	finally { _lock.Unlock(); };
	// (The active document cannot be asked for on this thread: the one Blender holds was the active one
	// when it was exported, and a modal loop does not change documents.)
	if (!_worker || _state != IPRSTATE::RUNNING || !doc || doc != _document)
		return false;
	// A step that failed on this thread: nothing more until the main thread has taken the failure over.
	if (_deferredFailure.IsPopulated())
		return true;
	const maxon::TimeValue started = maxon::TimeValue::GetTime();
	finally { TraceIpr("follow_ms", (maxon::TimeValue::GetTime() - started).GetMilliseconds()); };
	TraceIpr("evaluated");
	if (optimized)
	{
		FollowDocument(doc, false, true);
		return true;
	}
	if (_worker->GetPendingCount() != 0)
		return true;
	iferr (SyncLights(doc))
		Fail(err.GetMessage());
	return true;
}

void IprController::FollowDocument(cinema::BaseDocument* doc, maxon::Bool allowWhole, maxon::Bool cachesReadable)
{
	_activityTime = platform::UnixTime();
	if (_sceneDirty && allowWhole)
	{
		// A whole export is due anyway (an edit before this step).
		Tick();
		return;
	}
	// While Blender is still busy with the last step, wait: the next one covers this one.
	if (_worker->GetPendingCount() != 0)
		return;

	const DocumentCounters counters = ReadCounters(doc);
	_wholeNeeded = false;
	// After a tool's step the document is evaluated (no counter of the document moves then): what that
	// built is looked at once more, by the first call that may read it.
	const maxon::Bool again = _recheck && cachesReadable && !allowWhole;
	if (!(counters == _counters) || again)
	{
		if (again)
			_recheck = false;
		iferr (const ALONE alone = SyncAlone(doc, counters, cachesReadable))
		{
			Fail(err.GetMessage());
			return;
		}
		_wholeNeeded = alone == ALONE::NO;
		if (alone == ALONE::NO)
		{
			TraceIpr("step_whole");
			MarkSceneChanged();
			if (allowWhole)
			{
				Tick();
				return;
			}
		}
		else if (alone == ALONE::YES && allowWhole)
		{
			_recheck = true;
		}
		if (alone != ALONE::YES)
		{
			// Inside an evaluation the whole export waits for the end of the change, and what could not be
			// collected for a later step: the lights go on meanwhile.
			iferr (SyncLights(doc))
			{
				Fail(err.GetMessage());
				return;
			}
		}
	}
	iferr (SyncCamera(doc))
		Fail(err.GetMessage());
}

maxon::Result<void> IprController::FollowRedraw(cinema::BaseDocument* doc)
{
	iferr_scope;
	// (For comparisons, g_cyclesRenderIprUnoptimized: every redraw exports the scene whole.)
	if (IsIprUnoptimized())
		return SyncScene(false);
	const DocumentCounters counters = ReadCounters(doc);
	_wholeNeeded = false;
	const maxon::Bool optimized = GetIprLiveUpdates() && GetIprOptimizedUpdates();
	const maxon::Bool again = _recheck && optimized;
	_recheck = false;
	if (counters == _counters && !again)
	{
		// No counter moved since the last export or update: the redraw came from something that is not
		// in the scene (the view navigated, a manager). The settle export looks again.
		++_gatedCount;
		TraceIpr("gated");
		_settlePending = true;
		_dirtySince = 0.0;
		_lastChangeCount = 0;
		_lastStats = ExportStats();
		_lastExportMilliseconds = 0.0;
		return SyncView(doc);
	}
	if (optimized)
	{
		const ALONE alone = SyncAlone(doc, counters, true) iferr_return;
		_wholeNeeded = alone == ALONE::NO;
		// (Postponed: the next redraw, or the settle export, brings it.)
		if (alone != ALONE::NO)
			return SyncView(doc);
	}
	return SyncScene(false);
}

// --- results --------------------------------------------------------------------------------

maxon::Bool IprController::ReadFrame(Frame& frame, maxon::Float64 minSeconds)
{
	const IprLocked held(_lock);
	if (!_watcher || !_watcher->Take(frame, _frameArrival, minSeconds))
		return false;

	// Outside a render region Blender's viewport shows whatever it drew before: make it black.
	maxon::Int32 x0 = 0;
	maxon::Int32 y0 = 0;
	maxon::Int32 x1 = 0;
	maxon::Int32 y1 = 0;
	if (GetRegion(x0, y0, x1, y1) && frame.width == _width && frame.height == _height &&
		frame.pixels.GetCount() == maxon::Int(frame.width) * frame.height * 4)
	{
		for (maxon::Int32 y = 0; y < frame.height; ++y)
		{
			maxon::Float32* const row = frame.pixels.GetFirst() + maxon::Int(y) * frame.width * 4;
			const maxon::Bool inside = y >= y0 && y < y1;
			const maxon::Int32 leftEnd = inside ? maxon::Min(x0, frame.width) : frame.width;
			for (maxon::Int i = 0; i < maxon::Int(leftEnd) * 4; ++i)
				row[i] = (i & 3) == 3 ? 1.0f : 0.0f;
			if (inside)
			{
				for (maxon::Int i = maxon::Int(maxon::Max(x1, maxon::Int32(0))) * 4; i < maxon::Int(frame.width) * 4; ++i)
					row[i] = (i & 3) == 3 ? 1.0f : 0.0f;
			}
		}
	}
	return true;
}

maxon::Float64 IprController::GetElapsedSeconds() const
{
	const FrameStatus status = _reader.GetStatus();
	if (status.state == VIEWSTATE::STOPPED || status.startTime <= 0.0)
		return 0.0;
	const maxon::Float64 end = status.endTime > 0.0 ? status.endTime : platform::UnixTime();
	return end > status.startTime ? end - status.startTime : 0.0;
}

IprMemory IprController::GetMemory()
{
	const IprLocked held(_lock);
	const maxon::Float64 now = platform::UnixTime();
	if (now - _memoryTime >= 1.0 || now < _memoryTime)
	{
		_memoryTime = now;
		platform::MemoryStats stats;
		platform::QueryMemoryStats(_processId, stats);
		_memory.ram = stats.processRam;
		_memory.ramTotal = stats.totalRam;
		_memory.vram = stats.processVram;
		_memory.vramTotal = stats.totalVram;
	}
	return _memory;
}

} // namespace cyclesrender
