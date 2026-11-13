// SPDX-License-Identifier: Apache-2.0
// The IPR window: its controls, menu and timer, and the command that opens it (ipr_dialog.h).
//
// The window owns no render state. Every control calls the IprController, and a timer calls
// IprController::Tick() and refreshes what the window shows.

#include "c4d.h"
#include "c4d_colors.h"
#include "c4d_commanddata.h"
#include "c4d_gui.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "customgui_bitmapbutton.h"

#include "vpcyclesrender.h"

#include "../ids.h"
#include "ipr_add.h"
#include "ipr_controller.h"
#include "ipr_dialog.h"
#include "ipr_image_area.h"
#include "ipr_trace.h"

namespace cyclesrender
{

// Cinema 4D's own "Edit Render Settings..." command.
static const cinema::Int32 COMMAND_EDIT_RENDER_SETTINGS = 12161;

// How often the window looks at the controller: it shows a new frame, follows the camera and
// refreshes the status line at this pace.
static const cinema::Int32 TIMER_MILLISECONDS = 15;

enum
{
	ID_BUTTON_START = 1000,
	ID_BUTTON_PAUSE,
	ID_BUTTON_RELOAD,
	ID_COMBO_CAMERA,
	ID_BUTTON_LOCK,
	ID_BUTTON_SNAPSHOT,
	ID_EDIT_ZOOM,
	ID_BUTTON_FRAME,
	ID_COMBO_PASS,
	ID_COMBO_ENGINE,
	ID_BUTTON_SETTINGS,
	ID_AREA_IMAGE,
	ID_TEXT_TIME,
	ID_TEXT_RAM,
	ID_TEXT_VRAM,
	ID_AREA_PROGRESS,
	ID_GROUP_TOOLBAR,
	ID_GROUP_STATUS,

	ID_MENU_SNAPSHOT = 2000,
	ID_MENU_SAVE,
	ID_MENU_CLOSE,
	ID_MENU_SCALE_25,
	ID_MENU_SCALE_50,
	ID_MENU_SCALE_100,
	ID_MENU_SCALE_200,
	ID_MENU_SHOW_BLENDER,
	ID_MENU_RESTART_BLENDER,
	ID_MENU_FIT_WINDOW,
	ID_MENU_ADD_MATERIAL,
	ID_MENU_ADD_CAMERA,
	ID_MENU_ADD_CAMERA_TAG,
	ID_MENU_ADD_OBJECT_TAG,
	ID_MENU_ADD_LIGHT,
	ID_MENU_ADD_LIGHT_TAG,

	CAMERA_RENDER_VIEW = 0,
	CAMERA_AUTO = 1,
	CAMERA_FIRST_OBJECT = 2
};

enum class ICON
{
	PLAY,
	STOP,
	PAUSE,
	PAUSE_ACTIVE,
	RELOAD,
	LOCK_OPEN,
	LOCK_CLOSED,
	SNAPSHOT,
	FRAME,
	SETTINGS,
	COUNT
} MAXON_ENUM_LIST(ICON);

static const maxon::Char* const ICON_FILES[] = { "play.png", "stop.png", "pause.png", "pause_active.png", "reload.png", "lock_open.png", "lock_closed.png",
	"snapshot.png", "frame.png", "settings.png" };

static cinema::BaseBitmap* g_icons[maxon::Int(ICON::COUNT)] = {};

static cinema::BaseBitmap* GetIconBitmap(ICON icon)
{
	cinema::BaseBitmap*& bitmap = g_icons[maxon::Int(icon)];
	if (!bitmap)
	{
		bitmap = cinema::BaseBitmap::Alloc();
		const cinema::Filename file = cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename(maxon::String(ICON_FILES[maxon::Int(icon)]));
		if (bitmap && bitmap->Init(file) != cinema::IMAGERESULT::OK)
			cinema::BaseBitmap::Free(bitmap);
	}
	return bitmap;
}

static maxon::String TwoDigits(maxon::Int value)
{
	return (value < 10 ? "0"_s : ""_s) + maxon::String::IntToString(value);
}

// "2.5/16 GB": the part in use with one decimal, the total rounded to whole gigabytes.
static maxon::String MemoryText(maxon::Int64 used, maxon::Int64 total)
{
	const maxon::Float64 gigabyte = 1024.0 * 1024.0 * 1024.0;
	const maxon::String usedText = maxon::String::FloatToString(maxon::Float64(used) / gigabyte, 1, 1);
	if (total <= 0)
		return usedText + " GB"_s;
	return usedText + "/"_s + maxon::String::IntToString(maxon::Int64(maxon::Float64(total) / gigabyte + 0.5)) + " GB"_s;
}

static void CollectCameras(cinema::BaseObject* first, maxon::BaseArray<cinema::BaseObject*>& cameras)
{
	for (cinema::BaseObject* op = first; op; op = op->GetNext())
	{
		// Orscamera is the camera Cinema 4D creates by default; it is no instance of Ocamera.
		if (op->IsInstanceOf(Ocamera) || op->GetType() == Orscamera)
			cameras.Append(op) iferr_ignore("A camera missing from the list can still be chosen after the next change.");
		CollectCameras(op->GetDown(), cameras);
	}
}

//----------------------------------------------------------------------------------------
/// The bar at the right of the status line: "512 / 1024 Samples" over a bar filled to match.
//----------------------------------------------------------------------------------------
class IprProgressArea : public cinema::GeUserArea
{
public:
	virtual cinema::Bool GetMinSize(cinema::Int32& w, cinema::Int32& h) override
	{
		w = 290;
		h = 22;
		return true;
	}

	virtual void DrawMsg(cinema::Int32 x1, cinema::Int32 y1, cinema::Int32 x2, cinema::Int32 y2, const cinema::BaseContainer& msg) override
	{
		OffScreenOn();
		const cinema::Int32 width = GetWidth();
		const cinema::Int32 height = GetHeight();
		const cinema::Vector background(0.11);
		const cinema::Vector bar(0.17, 0.30, 0.56);
		DrawSetPen(background);
		DrawRectangle(0, 0, width, height);
		const cinema::Int32 filled = cinema::Int32(maxon::Float64(width) * maxon::Clamp01(_fraction) + 0.5);
		if (filled > 0)
		{
			DrawSetPen(bar);
			DrawRectangle(0, 0, filled, height);
		}
		// The text sits on the bar where the bar is, and on the background beyond it. Drawn with
		// an opaque background, the text would paint its whole box in one of the two colours:
		// wherever the bar ended inside the text, its rows showed the bar ending somewhere else
		// than the rows above them (seen: the bar reached to the end of the text there).
		DrawSetTextCol(cinema::Vector(0.9), COLOR_TRANS);
		DrawText(_text, 8, height / 2, DRAWTEXT_HALIGN_LEFT | DRAWTEXT_VALIGN_CENTER);
	}

	void Set(maxon::Int32 sample, maxon::Int32 samples)
	{
		const maxon::String text = samples > 0 ? FormatString("@ / @ Samples", sample, samples) : FormatString("@ Samples", sample);
		const maxon::Float64 fraction = samples > 0 ? maxon::Float64(sample) / maxon::Float64(samples) : 0.0;
		if (text == _text && fraction == _fraction)
			return;
		_text = text;
		_fraction = fraction;
		Redraw();
	}

private:
	maxon::String _text = "0 Samples"_s;
	maxon::Float64 _fraction = 0.0;
};

//----------------------------------------------------------------------------------------
/// The window. It owns no render state: every control calls the IprController, and a timer
/// shows what the controller reports.
//----------------------------------------------------------------------------------------
class IprDialog : public cinema::GeDialog
{
public:
	virtual cinema::Bool CreateLayout() override;
	virtual cinema::Bool InitValues() override;
	virtual cinema::Bool Command(cinema::Int32 id, const cinema::BaseContainer& msg) override;
	virtual cinema::Bool CoreMessage(cinema::Int32 id, const cinema::BaseContainer& msg) override;
	virtual void Timer(const cinema::BaseContainer& msg) override;
	virtual cinema::Bool AskClose() override;

	IprImageArea& GetImage() { return _image; }
	void SendSnapshot();
	maxon::Bool SaveImage(const cinema::Filename& file);
	void SetZoomPercent(maxon::Float64 percent);
	/// For tests: puts @p percent into the zoom field and reacts as to a value the user typed.
	void TypeZoomPercent(maxon::Float64 percent);
	/// The value the zoom field holds (1 = 100 %).
	maxon::Float64 GetZoomField() const;
	/// For tests: the number of cameras the dropdown lists, and what choosing one of them does.
	maxon::Int GetCameraCount() const { return _cameraIds.GetCount(); }
	void PickCamera(maxon::Int index)
	{
		RefreshCameras(true);
		SetInt32(ID_COMBO_CAMERA, CAMERA_FIRST_OBJECT + cinema::Int32(index));
		Command(ID_COMBO_CAMERA, cinema::BaseContainer());
	}
	void ShowNewFrame();

private:
	cinema::BitmapButtonCustomGui* AddIconButton(cinema::Int32 id, ICON icon, const maxon::String& tooltip, maxon::Bool toggle = false);
	void SetButtonIcon(cinema::BitmapButtonCustomGui* button, ICON icon);
	void RefreshCameras(maxon::Bool force);
	void RefreshPasses(maxon::Bool force);
	void RefreshControls(maxon::Bool force);
	void RefreshStatus();
	void RefreshMenu();
	void ShowZoom();

	IprImageArea _image;
	IprProgressArea _progress;
	cinema::BitmapButtonCustomGui* _startButton = nullptr;
	cinema::BitmapButtonCustomGui* _pauseButton = nullptr;
	cinema::BitmapButtonCustomGui* _lockButton = nullptr;

	Frame _incoming;
	maxon::BaseArray<maxon::UInt64> _cameraIds;	///< GUIDs of the cameras in the dropdown, in its order
	maxon::String _cameraNames;									///< their names joined, to notice a changed list
	maxon::Int32 _shownEngine = NOTOK;
	maxon::Int32 _shownPassCount = NOTOK;
	IPRSTATE _shownState = IPRSTATE::FAILED;
	maxon::Bool _shownLock = false;
	maxon::Bool _controlsShown = false;
	maxon::String _shownTime;
	maxon::String _shownRam;
	maxon::String _shownVram;
	maxon::Float64 _shownZoom = 0.0;	///< the zoom last written into the zoom field
	maxon::Int _ticks = 0;
	maxon::Float64 _lastTimer = 0.0;	///< when Timer() last ran (GeGetMilliSeconds); see CoreMessage()
};

static IprDialog* g_dialog = nullptr;

cinema::BitmapButtonCustomGui* IprDialog::AddIconButton(cinema::Int32 id, ICON icon, const maxon::String& tooltip, maxon::Bool toggle)
{
	cinema::BaseContainer settings;
	settings.SetBool(BITMAPBUTTON_BUTTON, true);
	settings.SetBool(BITMAPBUTTON_TOGGLE, toggle);
	settings.SetInt32(BITMAPBUTTON_BORDER, cinema::BORDER_NONE);
	settings.SetBool(BITMAPBUTTON_NOBORDERDRAW, true);
	settings.SetString(BITMAPBUTTON_TOOLTIP, tooltip);
	settings.SetInt32(BITMAPBUTTON_FORCE_SIZE, 24);
	cinema::BitmapButtonCustomGui* const button = static_cast<cinema::BitmapButtonCustomGui*>(
		AddCustomGui(id, CUSTOMGUI_BITMAPBUTTON, maxon::String(), cinema::BFH_LEFT | cinema::BFV_CENTER, 0, 0, settings));
	SetButtonIcon(button, icon);
	return button;
}

void IprDialog::SetButtonIcon(cinema::BitmapButtonCustomGui* button, ICON icon)
{
	cinema::BaseBitmap* const bitmap = GetIconBitmap(icon);
	if (!button || !bitmap)
		return;
	button->SetImage(bitmap, true);
	button->Redraw();
}

cinema::Bool IprDialog::CreateLayout()
{
	SetTitle("Cycles Render - IPR"_s);

	MenuFlushAll();
	MenuSubBegin("File"_s);
	MenuAddString(ID_MENU_SNAPSHOT, "Send Snapshot to Picture Viewer"_s);
	MenuAddString(ID_MENU_SAVE, "Save Image As..."_s);
	MenuAddSeparator();
	MenuAddString(ID_MENU_CLOSE, "Close"_s);
	MenuSubEnd();
	// What the plugin offers, added to the active document (ipr_add.h).
	MenuSubBegin("Add"_s);
	MenuAddString(ID_MENU_ADD_MATERIAL, "Cycles Material"_s);
	MenuAddSeparator();
	MenuAddString(ID_MENU_ADD_CAMERA, "Camera with Cycles Camera Tag"_s);
	MenuAddString(ID_MENU_ADD_CAMERA_TAG, "Cycles Camera Tag on Selected Cameras"_s);
	MenuAddString(ID_MENU_ADD_OBJECT_TAG, "Cycles Object Tag on Selected Objects"_s);
	MenuAddSeparator();
	MenuAddString(ID_MENU_ADD_LIGHT, "Cycles Light"_s);
	MenuAddString(ID_MENU_ADD_LIGHT_TAG, "Cycles Light Tag on Selected Lights"_s);
	MenuSubEnd();
	MenuSubBegin("Options"_s);
	MenuSubBegin("IPR Resolution"_s);
	MenuAddString(ID_MENU_FIT_WINDOW, "Fit Window"_s);
	MenuAddSeparator();
	MenuAddString(ID_MENU_SCALE_25, "25 % of the Render Settings"_s);
	MenuAddString(ID_MENU_SCALE_50, "50 % of the Render Settings"_s);
	MenuAddString(ID_MENU_SCALE_100, "100 % of the Render Settings"_s);
	MenuAddString(ID_MENU_SCALE_200, "200 % of the Render Settings"_s);
	MenuSubEnd();
	MenuAddSeparator();
	MenuAddString(ID_MENU_SHOW_BLENDER, "Show Blender Window"_s);
	MenuAddString(ID_MENU_RESTART_BLENDER, "Restart Blender"_s);
	MenuSubEnd();
	MenuFinished();

	GroupBegin(ID_GROUP_TOOLBAR, cinema::BFH_SCALEFIT, 0, 1, maxon::String(), 0);
	{
		GroupBorderSpace(6, 3, 6, 3);
		GroupSpace(6, 0);
		_startButton = AddIconButton(ID_BUTTON_START, ICON::PLAY, "Start or stop the render"_s);
		_pauseButton = AddIconButton(ID_BUTTON_PAUSE, ICON::PAUSE, "Pause the render and keep the scene; click again to resume"_s, true);
		AddIconButton(ID_BUTTON_RELOAD, ICON::RELOAD, "Reload the scene and restart the render"_s);
		AddStaticText(0, cinema::BFH_LEFT, 8, 0, maxon::String(), 0);
		// allowFiltering: with the list open, typing narrows it, for scenes with many cameras.
		AddComboBox(ID_COMBO_CAMERA, cinema::BFH_LEFT, 170, 0, false, true);
		_lockButton = AddIconButton(ID_BUTTON_LOCK, ICON::LOCK_OPEN, "Lock the camera: the render keeps the view it has now"_s);
		AddStaticText(0, cinema::BFH_LEFT, 8, 0, maxon::String(), 0);
		AddIconButton(ID_BUTTON_SNAPSHOT, ICON::SNAPSHOT, "Send the picture as it is now to the Picture Viewer"_s);
		AddStaticText(0, cinema::BFH_LEFT, 8, 0, maxon::String(), 0);
		AddEditNumberArrows(ID_EDIT_ZOOM, cinema::BFH_LEFT, 70, 0);
		AddIconButton(ID_BUTTON_FRAME, ICON::FRAME, "Show the whole picture, centred"_s);
		AddStaticText(0, cinema::BFH_SCALEFIT, 0, 0, maxon::String(), 0);
		AddComboBox(ID_COMBO_PASS, cinema::BFH_RIGHT, 170, 0);
		AddComboBox(ID_COMBO_ENGINE, cinema::BFH_RIGHT, 90, 0);
		AddIconButton(ID_BUTTON_SETTINGS, ICON::SETTINGS, "Open the Render Settings"_s);
	}
	GroupEnd();

	AddUserArea(ID_AREA_IMAGE, cinema::BFH_SCALEFIT | cinema::BFV_SCALEFIT, 0, 0);
	AttachUserArea(_image, ID_AREA_IMAGE);

	GroupBegin(ID_GROUP_STATUS, cinema::BFH_SCALEFIT, 0, 1, maxon::String(), 0);
	{
		GroupBorderSpace(6, 2, 6, 3);
		GroupSpace(12, 0);
		AddStaticText(ID_TEXT_TIME, cinema::BFH_LEFT, 170, 0, "Time:  00:00:00"_s, 0);
		AddStaticText(ID_TEXT_RAM, cinema::BFH_LEFT, 190, 0, "RAM:  0.0 GB"_s, 0);
		AddStaticText(ID_TEXT_VRAM, cinema::BFH_LEFT, 190, 0, "VRAM:  0.0 GB"_s, 0);
		AddStaticText(0, cinema::BFH_SCALEFIT, 0, 0, maxon::String(), 0);
		AddUserArea(ID_AREA_PROGRESS, cinema::BFH_RIGHT, 290, 0);
		AttachUserArea(_progress, ID_AREA_PROGRESS);
	}
	GroupEnd();
	return true;
}

cinema::Bool IprDialog::InitValues()
{
	AddChild(ID_COMBO_ENGINE, CYCLES_ENGINE_CYCLES, "Cycles"_s);
	AddChild(ID_COMBO_ENGINE, CYCLES_ENGINE_EEVEE, "Eevee"_s);
	ShowZoom();

	// Before the first start: show the engine and passes the Render Settings ask for.
	IprController& ipr = IprController::Get();
	ipr.SetEngineOverride(ipr.GetEngineOverride());

	_controlsShown = false;
	RefreshCameras(true);
	RefreshPasses(true);
	RefreshControls(true);
	RefreshMenu();
	SetTimer(TIMER_MILLISECONDS);
	return true;
}

void IprDialog::RefreshMenu()
{
	IprController& ipr = IprController::Get();
	const maxon::Bool fit = ipr.IsFitWindow();
	const maxon::Float64 scale = ipr.GetResolutionScale();
	MenuInitString(ID_MENU_FIT_WINDOW, true, fit);
	MenuInitString(ID_MENU_SCALE_25, true, !fit && maxon::Abs(scale - 0.25) < 0.001);
	MenuInitString(ID_MENU_SCALE_50, true, !fit && maxon::Abs(scale - 0.5) < 0.001);
	MenuInitString(ID_MENU_SCALE_100, true, !fit && maxon::Abs(scale - 1.0) < 0.001);
	MenuInitString(ID_MENU_SCALE_200, true, !fit && maxon::Abs(scale - 2.0) < 0.001);
	MenuInitString(ID_MENU_SHOW_BLENDER, true, ipr.IsBlenderWindowShown());
}

void IprDialog::RefreshCameras(maxon::Bool force)
{
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	maxon::BaseArray<cinema::BaseObject*> cameras;
	if (doc)
		CollectCameras(doc->GetFirstObject(), cameras);

	maxon::String names;
	for (cinema::BaseObject* camera : cameras)
		names += FormatString("@\n@\n", camera->GetName(), camera->GetGUID());

	IprController& ipr = IprController::Get();
	if (force || names != _cameraNames)
	{
		_cameraNames = names;
		_cameraIds.Reset();
		FreeChildren(ID_COMBO_CAMERA);
		AddChild(ID_COMBO_CAMERA, CAMERA_RENDER_VIEW, "< Render View >"_s);
		AddChild(ID_COMBO_CAMERA, CAMERA_AUTO, "< Auto >"_s);
		for (cinema::BaseObject* camera : cameras)
		{
			AddChild(ID_COMBO_CAMERA, CAMERA_FIRST_OBJECT + cinema::Int32(_cameraIds.GetCount()), camera->GetName());
			_cameraIds.Append(camera->GetGUID()) iferr_ignore("The entry then selects nothing.");
		}
	}

	// Show what the controller uses; a chosen camera that was deleted falls back to the render view.
	cinema::Int32 selected = CAMERA_RENDER_VIEW;
	if (ipr.GetCameraMode() == IPRCAMERA::AUTO)
	{
		selected = CAMERA_AUTO;
	}
	else if (ipr.GetCameraMode() == IPRCAMERA::OBJECT)
	{
		const cinema::BaseObject* const camera = ipr.GetCameraObject(doc);
		maxon::Int index = NOTOK;
		for (maxon::Int i = 0; camera && i < _cameraIds.GetCount(); ++i)
		{
			if (_cameraIds[i] == camera->GetGUID())
				index = i;
		}
		if (index != NOTOK)
			selected = CAMERA_FIRST_OBJECT + cinema::Int32(index);
		else
			ipr.SetCamera(IPRCAMERA::RENDER_VIEW);
	}
	SetInt32(ID_COMBO_CAMERA, selected);
}

void IprDialog::RefreshPasses(maxon::Bool force)
{
	IprController& ipr = IprController::Get();
	const maxon::Block<const IprPass> passes = ipr.GetPasses();
	if (force || ipr.GetEngine() != _shownEngine || maxon::Int32(passes.GetCount()) != _shownPassCount)
	{
		_shownEngine = ipr.GetEngine();
		_shownPassCount = maxon::Int32(passes.GetCount());
		FreeChildren(ID_COMBO_PASS);
		for (maxon::Int i = 0; i < passes.GetCount(); ++i)
			AddChild(ID_COMBO_PASS, cinema::Int32(i), maxon::String(passes[i].label));
		SetInt32(ID_COMBO_ENGINE, _shownEngine);
	}
	SetInt32(ID_COMBO_PASS, ipr.GetPass());
}

void IprDialog::RefreshControls(maxon::Bool force)
{
	IprController& ipr = IprController::Get();
	const IPRSTATE state = ipr.GetState();
	if (force || !_controlsShown || state != _shownState)
	{
		const maxon::Bool stopped = state == IPRSTATE::STOPPED || state == IPRSTATE::FAILED;
		SetButtonIcon(_startButton, stopped ? ICON::PLAY : ICON::STOP);
		// A paused render shows on the button: white bars on blue.
		if (_pauseButton)
			_pauseButton->SetToggleState(state == IPRSTATE::PAUSED);
		SetButtonIcon(_pauseButton, state == IPRSTATE::PAUSED ? ICON::PAUSE_ACTIVE : ICON::PAUSE);
		Enable(ID_BUTTON_PAUSE, state == IPRSTATE::RUNNING || state == IPRSTATE::PAUSED);

		switch (state)
		{
			case IPRSTATE::STARTING: _image.SetMessage("Starting Blender..."_s); break;
			case IPRSTATE::FAILED: _image.SetMessage(ipr.GetMessage() + "  Click here or press Start to start again."_s); break;
			case IPRSTATE::STOPPED: _image.SetMessage(_image.GetFrame().frameId == 0 ? "Press Start to render the scene."_s : maxon::String()); break;
			default: _image.SetMessage(maxon::String()); break;
		}
		_shownState = state;
	}
	if (force || !_controlsShown || ipr.IsCameraLocked() != _shownLock)
	{
		_shownLock = ipr.IsCameraLocked();
		SetButtonIcon(_lockButton, _shownLock ? ICON::LOCK_CLOSED : ICON::LOCK_OPEN);
	}
	_controlsShown = true;
}

void IprDialog::RefreshStatus()
{
	IprController& ipr = IprController::Get();
	const FrameStatus status = ipr.GetStatus();

	const maxon::Int seconds = maxon::Int(ipr.GetElapsedSeconds());
	const maxon::String time = "Time:  "_s + TwoDigits(seconds / 3600) + ":"_s + TwoDigits((seconds / 60) % 60) + ":"_s + TwoDigits(seconds % 60);
	if (time != _shownTime)
	{
		_shownTime = time;
		SetString(ID_TEXT_TIME, time);
	}

	const IprMemory memory = ipr.GetMemory();
	const maxon::String ram = "RAM:  "_s + MemoryText(memory.ram, memory.ramTotal);
	if (ram != _shownRam)
	{
		_shownRam = ram;
		SetString(ID_TEXT_RAM, ram);
	}
	const maxon::String vram = "VRAM:  "_s + MemoryText(memory.vram, memory.vramTotal);
	if (vram != _shownVram)
	{
		_shownVram = vram;
		SetString(ID_TEXT_VRAM, vram);
	}

	_progress.Set(status.state == VIEWSTATE::STOPPED ? 0 : status.sample, status.state == VIEWSTATE::STOPPED ? 0 : status.samples);
}

// The field shows the picture's zoom, which the image area keeps in steps of 0.1 %
// (IprImageArea::SetZoom), so the two always agree and the field shows at most one decimal.
void IprDialog::ShowZoom()
{
	_shownZoom = _image.GetZoom();
	SetPercent(ID_EDIT_ZOOM, _shownZoom, 2.0, 6400.0, 5.0);
}

void IprDialog::ShowNewFrame()
{
	IprController& ipr = IprController::Get();
	if (!ipr.ReadFrame(_incoming))
		return;
	// A frame that was still under way when the render was stopped is not shown.
	if (ipr.GetState() == IPRSTATE::STOPPED)
		return;
	// With "Fit Window" the frame shows more than the camera's picture.
	const CameraFraming& framing = ipr.GetFraming();
	const maxon::Bool wider = framing.frameWidth < maxon::Float64(framing.width) - 0.5 || framing.frameHeight < maxon::Float64(framing.height) - 0.5;
	_image.SetFrame(_incoming, cinema::GetActiveDocument(), wider ? ipr.GetFrameAspect() : 0.0);
	ipr.NoteFrame(_image.GetFrame());
	TraceIpr("shown", maxon::Float64(_image.GetFrame().frameId));
}

void IprDialog::Timer(const cinema::BaseContainer& msg)
{
	TraceIpr("timer");
	_lastTimer = cinema::GeGetMilliSeconds();
	IprController& ipr = IprController::Get();
	// The render follows the size of the picture area ("Fit Window").
	ipr.SetWindowSize(_image.GetWidth(), _image.GetHeight());
	_image.SetFill(ipr.IsFitWindow());
	ipr.Tick();
	ShowNewFrame();	// normally the frame thread's message got here first

	RefreshControls(false);
	RefreshPasses(false);
	RefreshStatus();
	// The area frames the picture anew by itself when the window or the frame changes size.
	if (_image.GetZoom() != _shownZoom)
		ShowZoom();
	// The camera list follows the document; a few times a second is often enough.
	if (++_ticks % 20 == 0)
		RefreshCameras(false);
}

cinema::Bool IprDialog::CoreMessage(cinema::Int32 id, const cinema::BaseContainer& msg)
{
	// EVMSG_CHANGE: something in a document changed. EVMSG_DOCUMENTRECALCULATED: Cinema 4D has
	// evaluated the scene for its own redraw, so the caches the exporter reads are current.
	if (id == ID_CORE_MESSAGE)
	{
		// The frame thread has a new frame.
		TraceIpr("framemsg");
		ShowNewFrame();
		return true;
	}
	if (IsIprTracing())
	{
		if (id == EVMSG_CHANGE)
			TraceIpr("change");
		else if (id == EVMSG_DOCUMENTRECALCULATED)
			TraceIpr("recalc");
		else if (id == EVMSG_ASYNCEDITORMOVE)
			TraceIpr("editormove", maxon::Float64(reinterpret_cast<maxon::UInt>(msg.GetVoid(cinema::BFM_CORE_PAR1))));
		else
			TraceIpr("core", maxon::Float64(id));
	}
	// While Cinema 4D is in a modal loop, such as a tool's drag in a view, the window's timer does
	// not run (measured: not once during an emulated drag, so the IPR followed a drag only at its
	// end), but a tool tells the managers about every step at once (EVMSG_ASYNCEDITORMOVE, sent with
	// GeSyncMessage). Then the IPR does here what the timer would: it sends what changed while
	// Blender is idle and shows the newest frame. Any other message in such a loop moves the camera.
	if (cinema::GeGetMilliSeconds() - _lastTimer > 3.0 * TIMER_MILLISECONDS)
	{
		IprController& ipr = IprController::Get();
		if (id == EVMSG_ASYNCEDITORMOVE)
		{
			TraceIpr("drag_step");
			ipr.MarkSceneChanged();
			ipr.Tick();
		}
		else
		{
			ipr.FollowCamera();
		}
		ShowNewFrame();
	}
	if (id == EVMSG_CHANGE || id == EVMSG_DOCUMENTRECALCULATED)
	{
		IprController::Get().MarkSceneChanged();
		if (id == EVMSG_CHANGE)
		{
			RefreshCameras(false);
			if (IprController::Get().GetState() == IPRSTATE::STOPPED)
				IprController::Get().SetEngineOverride(IprController::Get().GetEngineOverride());
		}
	}
	return cinema::GeDialog::CoreMessage(id, msg);
}

void IprDialog::SendSnapshot()
{
	cinema::BaseBitmap* bitmap = _image.CreateFloatBitmap(cinema::GetActiveDocument());
	if (!bitmap)
		return;
	cinema::ShowBitmap(bitmap, "Cycles Render IPR"_s);
	cinema::BaseBitmap::Free(bitmap);
}

maxon::Bool IprDialog::SaveImage(const cinema::Filename& file)
{
	cinema::BaseBitmap* bitmap = _image.CreateFloatBitmap(cinema::GetActiveDocument());
	if (!bitmap)
		return false;
	const maxon::Bool saved = bitmap->Save(file, FILTER_EXR, nullptr, cinema::SAVEBIT::USE32BITCHANNELS) == cinema::IMAGERESULT::OK;
	cinema::BaseBitmap::Free(bitmap);
	return saved;
}

void IprDialog::SetZoomPercent(maxon::Float64 percent)
{
	_image.SetZoom(percent / 100.0);
	ShowZoom();
}

void IprDialog::TypeZoomPercent(maxon::Float64 percent)
{
	// What the field holds after typing, unrounded; then the message the field sends.
	SetPercent(ID_EDIT_ZOOM, percent / 100.0, 2.0, 6400.0, 5.0);
	Command(ID_EDIT_ZOOM, cinema::BaseContainer(cinema::BFM_ACTION));
}

maxon::Float64 IprDialog::GetZoomField() const
{
	maxon::Float value = 0.0;
	return GetFloat(ID_EDIT_ZOOM, value) ? maxon::Float64(value) : 0.0;
}

cinema::Bool IprDialog::Command(cinema::Int32 id, const cinema::BaseContainer& msg)
{
	IprController& ipr = IprController::Get();
	switch (id)
	{
		case ID_BUTTON_START:
		{
			if (ipr.GetState() == IPRSTATE::STOPPED || ipr.GetState() == IPRSTATE::FAILED)
				ipr.Start();
			else
				ipr.Stop();
			break;
		}
		case ID_BUTTON_PAUSE: ipr.TogglePause(); break;
		case ID_BUTTON_RELOAD: ipr.Reload(); break;
		case ID_BUTTON_LOCK: ipr.SetCameraLock(!ipr.IsCameraLocked()); break;
		case ID_BUTTON_SNAPSHOT:
		case ID_MENU_SNAPSHOT: SendSnapshot(); break;
		case ID_BUTTON_FRAME:
		{
			_image.FrameImage();
			ShowZoom();
			break;
		}
		case ID_BUTTON_SETTINGS: cinema::CallCommand(COMMAND_EDIT_RENDER_SETTINGS); break;

		case ID_EDIT_ZOOM:
		{
			// A typed 47.25 % becomes 47.3 % in the picture, and the field is set to what the picture
			// uses. While the arrows are dragged the field is left alone, so that the drag goes on
			// from its own value; the last message of a drag has BFM_ACTION_INDRAG false.
			maxon::Float zoom = 1.0;
			if (GetFloat(ID_EDIT_ZOOM, zoom))
				_image.SetZoom(zoom);
			if (msg.GetBool(cinema::BFM_ACTION_INDRAG))
				_shownZoom = _image.GetZoom();	// keeps Timer() from setting the field meanwhile
			else
				ShowZoom();
			break;
		}
		case ID_AREA_IMAGE:
		{
			const cinema::Int32 what = msg.GetInt32(cinema::BFM_ACTION_VALUE);
			if (what == IPR_IMAGE_ZOOM_CHANGED)
				ShowZoom();
			else if (what == IPR_IMAGE_CLICKED && ipr.GetState() == IPRSTATE::FAILED)
				ipr.Start();
			break;
		}
		case ID_COMBO_CAMERA:
		{
			cinema::Int32 selected = CAMERA_RENDER_VIEW;
			GetInt32(ID_COMBO_CAMERA, selected);
			if (selected == CAMERA_AUTO)
			{
				ipr.SetCamera(IPRCAMERA::AUTO);
			}
			else if (selected >= CAMERA_FIRST_OBJECT && selected - CAMERA_FIRST_OBJECT < _cameraIds.GetCount())
			{
				maxon::BaseArray<cinema::BaseObject*> cameras;
				cinema::BaseDocument* const doc = cinema::GetActiveDocument();
				if (doc)
					CollectCameras(doc->GetFirstObject(), cameras);
				cinema::BaseObject* chosen = nullptr;
				for (cinema::BaseObject* camera : cameras)
				{
					if (camera->GetGUID() == _cameraIds[selected - CAMERA_FIRST_OBJECT])
						chosen = camera;
				}
				ipr.SetCamera(IPRCAMERA::OBJECT, chosen);
			}
			else
			{
				ipr.SetCamera(IPRCAMERA::RENDER_VIEW);
			}
			break;
		}
		case ID_COMBO_PASS:
		{
			cinema::Int32 pass = 0;
			if (GetInt32(ID_COMBO_PASS, pass))
				ipr.SetPass(pass);
			break;
		}
		case ID_COMBO_ENGINE:
		{
			cinema::Int32 engine = CYCLES_ENGINE_CYCLES;
			if (GetInt32(ID_COMBO_ENGINE, engine))
			{
				// Choosing the engine of the Render Settings means "follow the Render Settings" again.
				cinema::BaseDocument* const doc = cinema::GetActiveDocument();
				cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
				cinema::Int32 fromSettings = CYCLES_ENGINE_CYCLES;
				for (cinema::BaseVideoPost* post = renderData ? renderData->GetFirstVideoPost() : nullptr; post; post = post->GetNext())
				{
					if (post->GetType() == ID_CYCLES_VIDEOPOST)
						fromSettings = post->GetDataInstanceRef().GetInt32(CYCLES_ENGINE, CYCLES_ENGINE_CYCLES);
				}
				ipr.SetEngineOverride(engine == fromSettings ? NOTOK : engine);
				RefreshPasses(false);
			}
			break;
		}

		case ID_MENU_SAVE:
		{
			cinema::Filename file;
			if (_image.GetFrame().frameId != 0 && file.FileSelect(cinema::FILESELECTTYPE::IMAGES, cinema::FILESELECT::SAVE, "Save IPR Image"_s, "exr"_s))
			{
				if (!SaveImage(file))
					cinema::MessageDialog("The image could not be saved."_s);
			}
			break;
		}
		case ID_MENU_CLOSE:
		{
			ipr.Stop();
			Close();
			break;
		}
		case ID_MENU_FIT_WINDOW: ipr.SetFitWindow(true); RefreshMenu(); break;
		case ID_MENU_ADD_MATERIAL:
		case ID_MENU_ADD_CAMERA:
		case ID_MENU_ADD_CAMERA_TAG:
		case ID_MENU_ADD_OBJECT_TAG:
		case ID_MENU_ADD_LIGHT:
		case ID_MENU_ADD_LIGHT_TAG:
		{
			const IPRADD what = id == ID_MENU_ADD_MATERIAL ? IPRADD::CYCLES_MATERIAL : id == ID_MENU_ADD_CAMERA ? IPRADD::CAMERA_WITH_TAG
				: id == ID_MENU_ADD_CAMERA_TAG ? IPRADD::CAMERA_TAG : id == ID_MENU_ADD_LIGHT ? IPRADD::CYCLES_LIGHT
				: id == ID_MENU_ADD_LIGHT_TAG ? IPRADD::LIGHT_TAG : IPRADD::OBJECT_TAG;
			maxon::String message;
			AddToDocument(cinema::GetActiveDocument(), what, message);
			cinema::StatusSetText("Cycles Render: "_s + message);
			break;
		}
		case ID_MENU_SCALE_25:
		case ID_MENU_SCALE_50:
		case ID_MENU_SCALE_100:
		case ID_MENU_SCALE_200:
		{
			ipr.SetFitWindow(false);
			ipr.SetResolutionScale(id == ID_MENU_SCALE_25 ? 0.25 : id == ID_MENU_SCALE_50 ? 0.5 : id == ID_MENU_SCALE_100 ? 1.0 : 2.0);
			RefreshMenu();
			break;
		}
		case ID_MENU_SHOW_BLENDER: ipr.ShowBlenderWindow(!ipr.IsBlenderWindowShown()); RefreshMenu(); break;
		case ID_MENU_RESTART_BLENDER:
		{
			const maxon::Bool wasRunning = ipr.GetState() != IPRSTATE::STOPPED && ipr.GetState() != IPRSTATE::FAILED;
			ipr.RestartBlender();
			if (wasRunning)
				ipr.Start();
			break;
		}
	}
	RefreshControls(false);
	return true;
}

cinema::Bool IprDialog::AskClose()
{
	// Closing the window ends the render; Blender stays for the next time the window opens.
	IprController::Get().Stop();
	return false;
}

// --- the command ----------------------------------------------------------------------------

class IprCommand : public cinema::CommandData
{
public:
	virtual cinema::Bool Execute(cinema::BaseDocument* doc, cinema::GeDialog* parentManager) override
	{
		OpenIprDialog();
		return true;
	}

	virtual cinema::Bool RestoreLayout(void* secret) override
	{
		if (!g_dialog)
		{
			iferr (g_dialog = NewObj(IprDialog))
				return false;
		}
		return g_dialog->RestoreLayout(ID_IPR_COMMAND, 0, secret);
	}
};

void OpenIprDialog()
{
	if (!g_dialog)
	{
		iferr (g_dialog = NewObj(IprDialog))
			return;
	}
	g_dialog->Open(cinema::DLG_TYPE::ASYNC, ID_IPR_COMMAND, -1, -1, 960, 600);
}

void FreeIprDialog()
{
	DeleteObj(g_dialog);
	for (cinema::BaseBitmap*& icon : g_icons)
		cinema::BaseBitmap::Free(icon);
}

maxon::Int GetIprDialogCameraCount()
{
	return g_dialog && g_dialog->IsOpen() ? g_dialog->GetCameraCount() : NOTOK;
}

const Frame* GetIprDialogFrame()
{
	return g_dialog && g_dialog->IsOpen() ? &g_dialog->GetImage().GetFrame() : nullptr;
}

void GetIprDialogInfo(maxon::Float64& zoom, maxon::Float64& zoomField, maxon::Float64& convertMilliseconds, maxon::Bool& open)
{
	open = g_dialog && g_dialog->IsOpen();
	zoom = g_dialog ? g_dialog->GetImage().GetZoom() : 0.0;
	zoomField = open ? g_dialog->GetZoomField() : 0.0;
	convertMilliseconds = g_dialog ? g_dialog->GetImage().GetLastConvertMilliseconds() : 0.0;
}

maxon::Bool RunIprDialogCommand(const maxon::String& command, const maxon::String& argument, maxon::String& problem)
{
	if (command == "open"_s)
	{
		OpenIprDialog();
		return true;
	}
	if (command != "zoom"_s && command != "type_zoom"_s && command != "fit"_s && command != "snapshot"_s && command != "save"_s && command != "click"_s &&
			command != "pick_camera"_s)
		return false;
	if (!g_dialog || !g_dialog->IsOpen())
	{
		problem = "the IPR window is not open"_s;
		return true;
	}

	if (command == "click"_s)
	{
		// The same call a click on the control makes.
		static const struct { const maxon::Char* name; cinema::Int32 id; } CONTROLS[] = {
			{ "start", ID_BUTTON_START }, { "pause", ID_BUTTON_PAUSE }, { "reload", ID_BUTTON_RELOAD }, { "lock", ID_BUTTON_LOCK },
			{ "snapshot", ID_BUTTON_SNAPSHOT }, { "frame", ID_BUTTON_FRAME }, { "settings", ID_BUTTON_SETTINGS },
			{ "close", ID_MENU_CLOSE }, { "restart_blender", ID_MENU_RESTART_BLENDER }, { "show_blender", ID_MENU_SHOW_BLENDER }, { "scale_50", ID_MENU_SCALE_50 }, { "fit_window", ID_MENU_FIT_WINDOW },
			{ "scale_100", ID_MENU_SCALE_100 }, { "add_material", ID_MENU_ADD_MATERIAL }, { "add_camera", ID_MENU_ADD_CAMERA },
			{ "add_camera_tag", ID_MENU_ADD_CAMERA_TAG }, { "add_object_tag", ID_MENU_ADD_OBJECT_TAG }, { "add_light", ID_MENU_ADD_LIGHT },
			{ "add_light_tag", ID_MENU_ADD_LIGHT_TAG } };
		for (const auto& control : CONTROLS)
		{
			if (argument == maxon::String(control.name))
			{
				g_dialog->Command(control.id, cinema::BaseContainer());
				return true;
			}
		}
		problem = "the window has no control of that name"_s;
	}
	else if (command == "zoom"_s || command == "type_zoom"_s)
	{
		maxon::Float64 percent = 100.0;
		iferr (percent = argument.ToFloat())
			percent = 100.0;
		if (command == "zoom"_s)
			g_dialog->SetZoomPercent(percent);
		else
			g_dialog->TypeZoomPercent(percent);
	}
	else if (command == "pick_camera"_s)
	{
		maxon::Int index = 0;
		iferr (index = argument.ToInt())
			index = 0;
		if (index < 0 || index >= g_dialog->GetCameraCount())
			problem = "the camera list has no such entry"_s;
		else
			g_dialog->PickCamera(index);
	}
	else if (command == "fit"_s)
	{
		g_dialog->Command(ID_BUTTON_FRAME, cinema::BaseContainer());
	}
	else if (command == "snapshot"_s)
	{
		g_dialog->SendSnapshot();
	}
	else if (!g_dialog->SaveImage(cinema::Filename(argument)))
	{
		problem = "the image could not be saved"_s;
	}
	return true;
}

cinema::Bool RegisterIprCommand()
{
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("cyclesrender.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);

	// The command shares the renderer's plugin id, which Cinema 4D allows because ids are kept
	// per plugin type (ids.h). Cinema 4D owns the command object once it is registered.
	IprCommand* command = nullptr;
	iferr (command = NewObj(IprCommand))
	{
		cinema::BaseBitmap::Free(icon);
		return false;
	}
	const maxon::Bool registered = cinema::RegisterCommandPlugin(ID_IPR_COMMAND, cinema::GeLoadString(IDS_IPR_COMMAND_NAME), 0, icon,
		"Opens the live render window of Cycles Render."_s, command);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
