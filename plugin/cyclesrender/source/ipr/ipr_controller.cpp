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

#include "../platform/process_stats.h"
#include "../platform/shared_memory.h"
#include "../scene/camera_export.h"
#include "../videopost/render_settings_map.h"
#include "ipr_controller.h"

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

static IprController* g_controller = nullptr;

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
	_sceneState.Reset();
	_cameraState.Reset();
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
	_sceneState.Reset();
	_cameraState.Reset();
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
	if (_cameraMode == IPRCAMERA::OBJECT)
	{
		if (cinema::BaseObject* const camera = GetCameraObject(doc))
			return camera;
	}
	else if (_cameraMode == IPRCAMERA::AUTO)
	{
		if (cinema::BaseDraw* const view = doc->GetActiveBaseDraw())
		{
			cinema::BaseObject* const sceneCamera = view->GetSceneCamera(doc);
			return sceneCamera ? sceneCamera : view->GetEditorCamera();
		}
	}
	return FindRenderCamera(doc);
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
	if (!_sceneDirty)
		_dirtySince = platform::UnixTime();
	_sceneDirty = true;
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

	// Render settings, when they differ from what Blender has.
	const cinema::BaseContainer data = GetDocumentRenderSettings(doc);
	{
		JsonWriter settings;
		maxon::BaseArray<maxon::String> warnings;
		const maxon::Int32 engine = WriteRenderSettings(settings, data, doc, _engineOverride, warnings);
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
	_exportSettings.eevee = _engine == CYCLES_ENGINE_EEVEE;	// the IPR's engine menu may override the render settings
	_exportSettings.camera = ResolveCamera(doc);
	_exportSettings.writeCamera = false;

	SceneExporter exporter(doc, _exportSettings);
	exporter.Collect() iferr_return;

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

	const maxon::Int changes = task->scene->KeepChanges(_sceneState) iferr_return;
	_lastChangeCount = maxon::Int32(changes);
	if (changes > 0 || clear)
	{
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

	// The camera's picture has the shape of the Render Settings resolution.
	maxon::Int32 width = 1280;
	maxon::Int32 height = 720;
	if (const cinema::RenderData* const renderData = doc->GetActiveRenderData())
	{
		width = maxon::Max(maxon::Int32(1), renderData->GetDataInstanceRef().GetInt32(RDATA_XRES, width));
		height = maxon::Max(maxon::Int32(1), renderData->GetDataInstanceRef().GetInt32(RDATA_YRES, height));
	}
	_frameAspect = maxon::Float64(width) / maxon::Float64(height);

	const maxon::Bool fit = _fitWindow && _windowWidth > 0 && _windowHeight > 0;
	if (fit)
	{
		width = maxon::ClampValue(_windowWidth, MIN_SIDE, MAX_SIDE);
		height = maxon::ClampValue(_windowHeight, MIN_SIDE, MAX_SIDE);
	}
	else
	{
		width = maxon::ClampValue(maxon::Int32(maxon::Float64(width) * _scale + 0.5), MIN_SIDE, MAX_SIDE);
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
	ExportCamera(*task->scene, Coords::FromDocument(doc), camera, _exportSettings.depthOfField, _exportSettings.eevee, _framing) iferr_return;
	// Said once per session, also when the camera itself did not change (another engine chosen).
	for (const maxon::String& warning : task->scene->GetWarnings())
		ReportOnce(warning);

	const maxon::Int changes = task->scene->KeepChanges(_cameraState, true) iferr_return;
	if (changes > 0 || !_sentView.IsEmpty())
	{
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
	else
	{
		// The window got another size, or the menu asked for one: no need to look at the scene.
		if (_sizeDirty && platform::UnixTime() - _windowSizeTime >= WINDOW_SIZE_SETTLE_SECONDS)
			SyncSize(doc);
		iferr (SyncCamera(doc))
			Fail(err.GetMessage());
	}
}

// --- results --------------------------------------------------------------------------------

maxon::Bool IprController::ReadFrame(Frame& frame)
{
	if (!_watcher || !_watcher->Take(frame, _frameArrival))
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
