// SPDX-License-Identifier: Apache-2.0
// The IPR's state machine and its delta sync (ipr_controller.h).
//
// Reading order: the commands the window's buttons call (Start, Stop, TogglePause, Reload),
// then "following the document". Tick() is called by the window's timer and decides what to
// send: SyncScene() after a change of the document, SyncCamera() and SyncSize() on every
// tick. Nothing here waits for Blender: requests are posted to the worker thread
// (ipr_worker.h), and frames come back through shared memory (session/frame_watcher.h).

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

static IprController* g_controller = nullptr;

// A Cycles render on the CPU in the IPR leaves the Preferences' "CPU Threads Left Free" to Cinema 4D
// (0.2.0, B11; prefs/settings.h): a fixed thread count of the logical processors less those, or the
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
// long as that lasts, on the GPU too (measured 2026-11-17, internal/b11/priority_probe.py: 0 of 79 moves
// answered within 10 s, also with Blender's main thread raised to 6 or 15), and the IPR's worker would
// wait for it. So with Cinema 4D's default (below normal) both share the class, and the threads a CPU render
// leaves free (GetIprThreadsFree) keep cores for Cinema 4D; with Cinema 4D at normal Blender is below it.
static maxon::Int32 BlenderPriorityRank(maxon::Int32 own)
{
	return own >= 2 ? own - 1 : own;
}

IprController& IprController::Get()
{
	if (!g_controller)
	{
		iferr (g_controller = NewObj(IprController))
			CriticalStop("Out of memory for the IPR controller.");
	}
	return *g_controller;
}

void IprController::Free()
{
	DeleteObj(g_controller);
}

IprController::~IprController()
{
	DeleteObj(_watcher);
	_reader.Close();
	DeleteObj(_worker);	// ends the thread and Blender
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
	_state = IPRSTATE::FAILED;
	_message = message;
	_viewStarted = false;
	_exportedTimeKnown = false;
	_sceneState.Reset();
	_cameraState.Reset();
	_lights.Reset();
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
	if (_state != IPRSTATE::STOPPED && _state != IPRSTATE::FAILED)
		return;

	// Before anything is started: a Blender that cannot be started as chosen is said as such, with
	// where to choose another (0.2.0, B7). The worker would find the same (BlenderProcess::Start).
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
	ForgetFollowed();
	_sentView = "?"_s;
	_sentRegion = "?"_s;
	_sentSettings = maxon::String();
}

void IprController::TogglePause()
{
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
	_sentSettings = maxon::String();
	iferr (SyncScene(true))
		Fail(err.GetMessage());
}

void IprController::SetCamera(IPRCAMERA mode, cinema::BaseObject* camera)
{
	_cameraMode = camera || mode != IPRCAMERA::OBJECT ? mode : IPRCAMERA::RENDER_VIEW;
	if (_cameraLink)
		_cameraLink->SetLink(_cameraMode == IPRCAMERA::OBJECT ? camera : nullptr);
}

cinema::BaseObject* IprController::GetCameraObject(cinema::BaseDocument* doc) const
{
	if (_cameraMode != IPRCAMERA::OBJECT || !_cameraLink || !doc)
		return nullptr;
	return static_cast<cinema::BaseObject*>(_cameraLink->GetLink(doc, Obase));
}

void IprController::SetCameraLock(maxon::Bool locked)
{
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

maxon::Block<const IprPass> IprController::GetPasses() const
{
	if (_engine == CYCLES_ENGINE_EEVEE)
		return maxon::Block<const IprPass>(EEVEE_PASSES, SIZEOF(EEVEE_PASSES) / SIZEOF(EEVEE_PASSES[0]));
	return maxon::Block<const IprPass>(CYCLES_PASSES, SIZEOF(CYCLES_PASSES) / SIZEOF(CYCLES_PASSES[0]));
}

void IprController::SetPass(maxon::Int32 index)
{
	_passIndex = index;
	if (_viewStarted)
		PostPass();
}

void IprController::SetEngineOverride(maxon::Int32 engine)
{
	_engineOverride = engine;
	_sceneDirty = true;
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
	_scale = maxon::ClampValue(scale, 0.05, 4.0);
	_sizeDirty = true;
	_windowSizeTime = 0.0;	// a choice from the menu takes effect at once
}

void IprController::SetFitWindow(maxon::Bool fit)
{
	_fitWindow = fit;
	_sizeDirty = true;
	_windowSizeTime = 0.0;
}

void IprController::SetWindowSize(maxon::Int32 width, maxon::Int32 height)
{
	if (width == _windowWidth && height == _windowHeight)
		return;
	_windowWidth = width;
	_windowHeight = height;
	_windowSizeTime = platform::UnixTime();
	_sizeDirty = true;
}

void IprController::RestartBlender()
{
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
	_windowShown = visible;
	PostSimple("view_window", "visible", visible);
}

void IprController::KillBlenderForTest()
{
	if (_worker)
		_worker->KillBlenderForTest();
}

void IprController::MarkSceneChanged()
{
	_activityTime = platform::UnixTime();
	if (!_sceneDirty)
		_dirtySince = _activityTime;
	_sceneDirty = true;
}

void IprController::NoteRedraw()
{
	_activityTime = platform::UnixTime();
	if (!_redrawn && !_sceneDirty)
		_dirtySince = _activityTime;
	_redrawn = true;
}

void IprController::ForgetFollowed()
{
	_nodes.Reset();
	_objects.Reset();
	_nodesValid = false;
	_counters = DocumentCounters();
	_redrawn = false;
	_settlePending = false;
	_wholeNeeded = false;
}

void IprController::NoteFrame(const Frame& frame)
{
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

maxon::Result<void> IprController::SyncScene(maxon::Bool clear)
{
	iferr_scope;

	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc)
		return maxon::IllegalStateError(MAXON_SOURCE_LOCATION, "There is no document to render."_s);
	_document = doc;
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

	const maxon::TimeValue started = maxon::TimeValue::GetTime();
	finally { TraceIpr("camera_ms", (maxon::TimeValue::GetTime() - started).GetMilliseconds()); };
	// A locked camera is a set of values that never changes (SetCameraLock); it is sent like
	// any other, which changes nothing in Blender unless the image changed its shape.
	CameraValues camera;
	if (_cameraLocked && _hasLockedCamera)
	{
		camera = _lockedCamera;
	}
	else
	{
		const cinema::BaseObject* const object = ResolveCamera(doc);
		if (!object)
			return maxon::OK;
		camera = CameraValues::Read(object);
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
	if (!_worker)
		return;
	TraceIpr("tick");

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

	// With the Preferences' live updates off (0.2.0, F22) nothing is sent while the timeline plays,
	// neither the scene nor an animated camera: the IPR shows the frame playback stopped at, as before
	// 0.2.0 for the scene (and so that the camera does not run ahead of the objects).
	if (!GetIprLiveUpdates() && cinema::CheckIsRunning(cinema::CHECKISRUNNING::ANIMATIONRUNNING))
	{
		TraceIpr("playback_held");
		return;
	}

	// Playback changes the document's time without any change event (measured: none in 3 s of
	// playing, an EVMSG_CHANGE only when it stopped), so the time is compared on every tick; the
	// window's timer runs while Cinema 4D plays. This also follows a time set without an event.
	if (_exportedTimeKnown && doc->GetTime() != _exportedTime)
		MarkSceneChanged();

	// While Blender is still busy with the last change, wait: the next export then covers
	// everything that happened in the meantime.
	if (!idle)
		return;

	if (_sceneDirty)
	{
		_sceneDirty = false;
		iferr (SyncScene(false))
			Fail(err.GetMessage());
	}
	else if (_redrawn)
	{
		_redrawn = false;
		iferr (FollowRedraw(doc))
			Fail(err.GetMessage());
	}
	else if (_settlePending && platform::UnixTime() - _activityTime >= SETTLE_SECONDS)
	{
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
	// Why the CPU (0.2.0, B11: the reported lag was a scene whose Render Settings said CPU while the
	// Preferences named a GPU type): the Render Settings ask for it, or no GPU could be used.
	return FormatString("Cycles on CPU, @ threads @", info.threads, _askedDevice == CYCLES_DEVICE_CPU ? "(Render Settings)"_s : "(no GPU found)"_s);
}

maxon::Result<maxon::String> IprController::VerifySync()
{
	iferr_scope;
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
	if (!_worker || _state != IPRSTATE::RUNNING || _worker->GetPendingCount() != 0)
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
	if (!_worker || _state != IPRSTATE::RUNNING || _worker->GetPendingCount() != 0)
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

// --- a change in progress: the gate, and what moves sent alone (0.2.0, B11) --------------------

IprController::DocumentCounters IprController::ReadCounters(cinema::BaseDocument* doc)
{
	DocumentCounters counters;
	counters.all = doc->GetHDirty(cinema::HDIRTYFLAGS::ALL);
	counters.time = doc->GetTime();
	counters.known = true;
	return counters;
}

maxon::UInt32 IprController::ReadOtherCounters(cinema::BaseDocument* doc)
{
	return doc->GetHDirty(cinema::HDIRTYFLAGS::ALL & ~(cinema::HDIRTYFLAGS::OBJECT | cinema::HDIRTYFLAGS::OBJECT_MATRIX | cinema::HDIRTYFLAGS::TAG));
}

// The data counter of an object with those of its tags (their number and order count too).
static maxon::UInt32 DataDirty(const cinema::BaseObject* op)
{
	maxon::UInt32 dirty = op->GetDirty(cinema::DIRTYFLAGS::DATA);
	for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
		dirty = dirty * 31 + tag->GetDirty(cinema::DIRTYFLAGS::DATA) + 1;
	return dirty;
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
// anew, or with @p deformed everything in the cache.
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

// The global matrix of an object as the exporter's walk forms it: the top object's local matrix times
// each child's, from the top down (SceneExporter::WriteLightAlone).
static cinema::Matrix WalkMatrix(const cinema::BaseObject* op)
{
	const cinema::BaseObject* const up = op->GetUp();
	return up ? WalkMatrix(up) * op->GetMl() : op->GetMl();
}

maxon::Result<void> IprController::NoteFollowed(cinema::BaseDocument* doc, SceneExporter& exporter)
{
	iferr_scope;
	_nodes.Reset();
	_objects.Reset();
	_nodesValid = false;
	if (!_exportSettings.noteFollowed)
		return maxon::OK;

	// The objects of the document, in the walk's order, with their counters as this export saw them.
	maxon::HashMap<const cinema::BaseObject*, maxon::Int32> nodeOf;
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
		node.dataDirty = DataDirty(op);
		node.cacheDirty = op->GetDirty(cinema::DIRTYFLAGS::CACHE);
		node.renderMode = op->GetRenderMode();
		const maxon::Int32 type = op->GetType();
		// A light's values are written with the light alone, a camera's are sent by SyncCamera().
		node.dataAlone = type == Olight || type == Orslight || type == ID_LIGHT_OBJECT || type == Orscamera || op->IsInstanceOf(Ocamera);
		nodeOf.Insert(op, maxon::Int32(_nodes.GetCount())) iferr_return;
		open.Append(maxon::Int32(_nodes.GetCount())) iferr_return;
		_nodes.Append(node) iferr_return;
	}
	for (const maxon::Int32 index : open)
		_nodes[index].end = maxon::Int32(_nodes.GetCount());

	for (const cinema::BaseObject* const owner : exporter.GetUnfollowedOwners())
	{
		if (const auto* const found = nodeOf.Find(owner))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			node.unfollowed = true;
			node.dataAlone = false;	// a dome light is the world, a light of a generator is not written alone
		}
	}
	for (const cinema::BaseObject* const owner : exporter.GetDeformedOwners())
	{
		if (const auto* const found = nodeOf.Find(owner))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			node.deformed = true;
			node.cacheDirty = CacheDirty(owner, true);
		}
	}
	for (const cinema::BaseObject* const root : exporter.GetReferenceRoots())
	{
		if (const auto* const found = nodeOf.Find(root))
		{
			for (maxon::Int32 index = found->GetValue() + 1; index < _nodes[found->GetValue()].end; ++index)
				_nodes[index].referenced = true;
		}
	}

	// The items, grouped by their owner's node.
	maxon::BaseArray<WrittenObject> written = exporter.TakeWrittenObjects();
	for (const WrittenObject& object : written)
	{
		if (const auto* const found = nodeOf.Find(object.owner))
			++_nodes[found->GetValue()].objectCount;
	}
	maxon::Int32 first = 0;
	for (FollowedNode& node : _nodes)
	{
		node.firstObject = first;
		first += node.objectCount;
		node.objectCount = 0;
	}
	_objects.Resize(first) iferr_return;
	for (WrittenObject& object : written)
	{
		if (const auto* const found = nodeOf.Find(object.owner))
		{
			FollowedNode& node = _nodes[found->GetValue()];
			_objects[node.firstObject + node.objectCount++] = std::move(object);
		}
	}
	_nodesValid = true;
	return maxon::OK;
}

maxon::Result<maxon::Bool> IprController::SyncAlone(cinema::BaseDocument* doc, const DocumentCounters& counters)
{
	iferr_scope;
	const maxon::TimeValue start = maxon::TimeValue::GetTime();
	_lastAloneObjects = 0;
	// Another time moves whatever is animated; anything but objects and tags (materials, render settings,
	// the hierarchy, layers, takes) is not looked at here.
	if (!_nodesValid || !_counters.known || counters.time != _counters.time || ReadOtherCounters(doc) != _otherCounters)
		return false;

	IprTask* task = NewObj(IprTask) iferr_return;
	finally
	{
		DeleteObj(task);
	};
	task->kind = IprTask::KIND::SCENE;
	task->scene = NewObj(SceneWriter, false) iferr_return;
	const Coords coords = Coords::FromDocument(doc);

	// Every object of the document against what the last whole export saw of it. `moving` holds, per
	// level of the walk, whether the object there or one above it moved, and `matrices` its matrix then.
	struct Seen
	{
		maxon::Int32 node;
		maxon::UInt32 matrixDirty;
		maxon::UInt32 dataDirty;
	};
	maxon::BaseArray<Seen> seen;
	maxon::BaseArray<maxon::Bool> moving;
	maxon::BaseArray<cinema::Matrix> matrices;
	maxon::Int depth = 0;
	maxon::Int32 index = 0;
	for (const cinema::BaseObject* op = doc->GetFirstObject(); op; op = NextInWalk(op, depth), ++index)
	{
		// An object added, removed or put elsewhere: the hierarchy counter said so already; a guard.
		if (index >= _nodes.GetCount() || _nodes[index].object != op)
			return false;
		const FollowedNode& node = _nodes[index];
		const maxon::UInt32 matrixDirty = op->GetDirty(cinema::DIRTYFLAGS::MATRIX);
		const maxon::UInt32 dataDirty = DataDirty(op);
		// A cache built anew (the object is a generator whose input changed, or is deformed), or the render
		// dot (which counts as a change of the matrix): not a change of place.
		if (CacheDirty(op, node.deformed) != node.cacheDirty || op->GetRenderMode() != node.renderMode)
			return false;
		if (dataDirty != node.dataDirty && !node.dataAlone)
			return false;
		const maxon::Bool movedItself = matrixDirty != node.matrixDirty;
		if (movedItself && node.referenced)
			return false;
		const maxon::Bool above = depth > 0 && moving[depth - 1];
		const maxon::Bool moves = movedItself || above;
		if (moves && node.unfollowed)
			return false;
		if (moving.GetCount() <= depth)
		{
			moving.Resize(depth + 1) iferr_return;
			matrices.Resize(depth + 1) iferr_return;
		}
		moving[depth] = moves;
		if (moves)
		{
			matrices[depth] = above ? matrices[depth - 1] * op->GetMl() : WalkMatrix(op);
			for (maxon::Int32 i = 0; i < node.objectCount; ++i)
				SceneExporter::WriteObjectAlone(*task->scene, coords, _objects[node.firstObject + i], matrices[depth]);
			_lastAloneObjects += node.objectCount;
		}
		if (movedItself || dataDirty != node.dataDirty)
		{
			seen.Append(Seen{ index, matrixDirty, dataDirty }) iferr_return;
		}
	}
	if (index != _nodes.GetCount())
		return false;

	// The lights, each by itself as since F18: their values and their places.
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
		return false;
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);

	const maxon::Int changes = task->scene->KeepChanges(_sceneState, true) iferr_return;
	for (const Seen& entry : seen)
	{
		_nodes[entry.node].matrixDirty = entry.matrixDirty;
		_nodes[entry.node].dataDirty = entry.dataDirty;
	}
	_counters = counters;
	_settlePending = true;
	_lastChangeCount = maxon::Int32(changes);
	_lastAloneMilliseconds = (maxon::TimeValue::GetTime() - start).GetMilliseconds();
	TraceIpr("alone_ms", _lastAloneMilliseconds);
	if (changes > 0)
	{
		++_aloneCount;
		TraceIpr("posted_alone", maxon::Float64(changes));
		_worker->Post(task);
		task = nullptr;
		ArmLatency(_dirtySince);
	}
	_dirtySince = 0.0;
	return true;
}

void IprController::FollowStep(maxon::Bool allowWhole)
{
	if (!_worker || _state != IPRSTATE::RUNNING)
		return;
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	if (!doc || doc != _document)
		return;
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
	if (!(counters == _counters))
	{
		iferr (const maxon::Bool alone = SyncAlone(doc, counters))
		{
			Fail(err.GetMessage());
			return;
		}
		_wholeNeeded = !alone;
		if (!alone)
		{
			TraceIpr("step_whole");
			MarkSceneChanged();
			if (allowWhole)
			{
				Tick();
				return;
			}
			// Inside an evaluation: the whole export waits for the end of the change, the lights go on.
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
	// (For comparisons: as before 0.2.0, every redraw exports.)
	if (IsIprUnoptimized())
		return SyncScene(false);
	const DocumentCounters counters = ReadCounters(doc);
	_wholeNeeded = false;
	if (counters == _counters)
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
	if (GetIprLiveUpdates() && GetIprOptimizedUpdates())
	{
		const maxon::Bool alone = SyncAlone(doc, counters) iferr_return;
		_wholeNeeded = !alone;
		if (alone)
			return SyncView(doc);
	}
	return SyncScene(false);
}

// --- results --------------------------------------------------------------------------------

maxon::Bool IprController::ReadFrame(Frame& frame, maxon::Float64 minSeconds)
{
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
