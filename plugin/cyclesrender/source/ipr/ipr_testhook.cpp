// SPDX-License-Identifier: Apache-2.0
// The IPR's test hook: commands from test scripts, and the status they get back as JSON
// (ipr_testhook.h). Only active with the development setting g_cyclesRenderIprTest.

#include "c4d.h"
#include "maxon/configuration.h"
#include "maxon/file_utilities.h"

#include "../ids.h"
#include "../session/json_writer.h"
#include "ipr_controller.h"
#include "ipr_dialog.h"
#include "ipr_testhook.h"
#include "../materials/native_material.h"

MAXON_CONFIGURATION_BOOL(g_cyclesRenderIprTest, false, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: accept IPR commands from test scripts through the plugin's preferences container.");

namespace cyclesrender
{

// Keys in the plugin's preferences container, next to the settings of prefs/settings.cpp.
static const maxon::Int32 KEY_COMMAND = 9000;
static const maxon::Int32 KEY_ANSWER = 9001;

static const maxon::Char* StateName(IPRSTATE state)
{
	switch (state)
	{
		case IPRSTATE::STOPPED: return "stopped";
		case IPRSTATE::STARTING: return "starting";
		case IPRSTATE::RUNNING: return "running";
		case IPRSTATE::PAUSED: return "paused";
		case IPRSTATE::FAILED: return "failed";
	}
	return "";
}

static const maxon::Char* ViewName(VIEWSTATE state)
{
	switch (state)
	{
		case VIEWSTATE::STOPPED: return "stopped";
		case VIEWSTATE::RENDERING: return "rendering";
		case VIEWSTATE::PAUSED: return "paused";
		case VIEWSTATE::DONE: return "done";
	}
	return "";
}

static cinema::BaseObject* FindCameraByName(cinema::BaseObject* first, const maxon::String& name)
{
	for (cinema::BaseObject* op = first; op; op = op->GetNext())
	{
		if ((op->GetType() == Ocamera || op->GetType() == Orscamera) && op->GetName() == name)
			return op;
		if (cinema::BaseObject* const found = FindCameraByName(op->GetDown(), name))
			return found;
	}
	return nullptr;
}

// The newest frame as "CRF1", width, height, then RGB float32 with the top row first: the
// format of tests/c4d/lib/crtest.py.
static maxon::Result<void> SaveFrame(const Frame& frame, const maxon::String& path)
{
	iferr_scope;

	maxon::BaseArray<maxon::Char> bytes;
	const maxon::Int pixelCount = maxon::Int(frame.width) * frame.height;
	bytes.Resize(12 + pixelCount * 3 * SIZEOF(maxon::Float32)) iferr_return;
	const maxon::UInt32 header[2] = { maxon::UInt32(frame.width), maxon::UInt32(frame.height) };
	maxon::MemCopyType(bytes.GetFirst(), "CRF1", 4);
	maxon::MemCopyType(bytes.GetFirst() + 4, reinterpret_cast<const maxon::Char*>(header), 8);
	maxon::Float32* const out = reinterpret_cast<maxon::Float32*>(bytes.GetFirst() + 12);
	for (maxon::Int i = 0; i < pixelCount; ++i)
	{
		out[i * 3 + 0] = frame.pixels[i * 4 + 0];
		out[i * 3 + 1] = frame.pixels[i * 4 + 1];
		out[i * 3 + 2] = frame.pixels[i * 4 + 2];
	}
	maxon::FileUtilities::WriteArray(maxon::Url(path), maxon::Block<maxon::Char>(bytes.GetFirst(), bytes.GetCount())) iferr_return;
	return maxon::OK;
}

static maxon::String Execute(const maxon::String& commandLine)
{
	IprController& ipr = IprController::Get();

	maxon::String command = commandLine;
	maxon::String argument;
	maxon::Int space = 0;
	if (commandLine.Find(" "_s, &space))
	{
		command = commandLine.GetLeftPart(space);
		argument = commandLine.GetRightPart(space + 1);
	}
	maxon::Int32 number = 0;
	iferr (number = argument.ToInt32())
		number = 0;	// not every command takes a number
	maxon::String problem;
	// The newest frame: the one the window shows, or, without a window, fetched here.
	static Frame fetched;
	const Frame* const shown = GetIprDialogFrame();
	if (!shown && ipr.ReadFrame(fetched))
		ipr.NoteFrame(fetched);
	const Frame& frame = shown ? *shown : fetched;

	if (RunIprDialogCommand(command, argument, problem))
	{
		// A command for the window; it has been carried out.
	}
	else if (command == "start"_s)
	{
		ipr.Start();
	}
	else if (command == "stop"_s)
	{
		ipr.Stop();
	}
	else if (command == "pause"_s)
	{
		ipr.TogglePause();
	}
	else if (command == "reload"_s)
	{
		ipr.Reload();
	}
	else if (command == "tick"_s)
	{
		ipr.Tick();
		// Without a window no timer runs either: the materials open in Blender's Shader Editor.
		NativeMaterialEditTick();
	}
	else if (command == "changed"_s)
	{
		ipr.MarkSceneChanged();
	}
	else if (command == "kill"_s)
	{
		ipr.KillBlenderForTest();
	}
	else if (command == "pass"_s)
	{
		ipr.SetPass(number);
	}
	else if (command == "engine"_s)
	{
		ipr.SetEngineOverride(number);
	}
	else if (command == "lock"_s)
	{
		ipr.SetCameraLock(number != 0);
	}
	else if (command == "window"_s)
	{
		ipr.ShowBlenderWindow(number != 0);
	}
	else if (command == "fitwindow"_s)
	{
		ipr.SetFitWindow(number != 0);
	}
	else if (command == "winsize"_s)
	{
		// "winsize 800x300": stands in for a window of that size when there is none.
		maxon::Int cross = 0;
		maxon::Int32 windowWidth = 0;
		maxon::Int32 windowHeight = 0;
		if (argument.Find("x"_s, &cross))
		{
			iferr (windowWidth = argument.GetLeftPart(cross).ToInt32())
				windowWidth = 0;
			iferr (windowHeight = argument.GetRightPart(cross + 1).ToInt32())
				windowHeight = 0;
		}
		ipr.SetWindowSize(windowWidth, windowHeight);
	}
	else if (command == "scale"_s)
	{
		maxon::Float64 scale = 1.0;
		iferr (scale = argument.ToFloat())
			scale = 1.0;
		ipr.SetResolutionScale(scale > 0.0 ? scale : 1.0);
	}
	else if (command == "camera"_s)
	{
		if (argument == "render"_s)
		{
			ipr.SetCamera(IPRCAMERA::RENDER_VIEW);
		}
		else if (argument == "auto"_s)
		{
			ipr.SetCamera(IPRCAMERA::AUTO);
		}
		else
		{
			cinema::BaseDocument* const doc = cinema::GetActiveDocument();
			cinema::BaseObject* const camera = doc ? FindCameraByName(doc->GetFirstObject(), argument) : nullptr;
			if (camera)
			{
				ipr.SetCamera(IPRCAMERA::OBJECT, camera);
			}
			else
			{
				problem = "no camera object of that name"_s;
			}
		}
	}
	else if (command == "frame"_s)
	{
		if (frame.frameId == 0)
		{
			problem = "no frame yet"_s;
		}
		else
		{
			iferr (SaveFrame(frame, argument))
				problem = err.GetMessage();
		}
	}
	else if (command == "free"_s)
	{
		FreeNativeMaterialEditing();   // the material editor's Blender as well
		IprController::Free();
		fetched = Frame();
		return "{\"state\":\"freed\"}"_s;
	}
	else if (command != "status"_s)
	{
		problem = "unknown command"_s;
	}

	maxon::Float64 mean[3] = { 0.0, 0.0, 0.0 };
	const maxon::Int pixelCount = maxon::Int(frame.width) * frame.height;
	// A coarse grid is enough to tell pictures apart, and keeps this cheap for large frames.
	const maxon::Int step = pixelCount > 40000 ? pixelCount / 40000 : 1;
	maxon::Int counted = 0;
	for (maxon::Int i = 0; i < pixelCount; i += step, ++counted)
	{
		mean[0] += frame.pixels[i * 4 + 0];
		mean[1] += frame.pixels[i * 4 + 1];
		mean[2] += frame.pixels[i * 4 + 2];
	}
	if (counted > 0)
	{
		mean[0] /= maxon::Float64(counted);
		mean[1] /= maxon::Float64(counted);
		mean[2] /= maxon::Float64(counted);
	}

	const FrameStatus status = ipr.GetStatus();
	const IprMemory memory = ipr.GetMemory();
	maxon::Int32 width = 0;
	maxon::Int32 height = 0;
	ipr.GetSize(width, height);

	JsonWriter json;
	json.BeginObject();
	json.String("state", maxon::String(StateName(ipr.GetState())));
	json.String("message", ipr.GetMessage());
	json.String("problem", problem);
	json.Bool("idle", ipr.IsIdle());
	json.Int("frame", maxon::Int64(status.frameId));
	json.String("view", maxon::String(ViewName(status.state)));
	json.Int("sample", status.sample);
	json.Int("samples", status.samples);
	json.Int("width", status.width);
	json.Int("height", status.height);
	json.Int("wanted_width", width);
	json.Int("wanted_height", height);
	const maxon::Float64 cameraFrame[2] = { ipr.GetFraming().frameWidth, ipr.GetFraming().frameHeight };
	json.Floats("camera_frame", cameraFrame, 2);
	json.Bool("fit_window", ipr.IsFitWindow());
	json.Int("window_cameras", GetIprDialogCameraCount());
	maxon::Int32 regionX0 = 0;
	maxon::Int32 regionY0 = 0;
	maxon::Int32 regionX1 = 0;
	maxon::Int32 regionY1 = 0;
	if (ipr.GetRegion(regionX0, regionY0, regionX1, regionY1))
	{
		const maxon::Float64 region[4] = { maxon::Float64(regionX0), maxon::Float64(regionY0), maxon::Float64(regionX1), maxon::Float64(regionY1) };
		json.Floats("region", region, 4);
	}
	json.Int("frame_width", frame.width);
	json.Int("frame_height", frame.height);
	json.Float("elapsed", ipr.GetElapsedSeconds());
	json.Int("scenes_sent", ipr.GetSentSceneCount());
	json.Int("requests_sent", ipr.GetSentRequestCount());
	json.Int("changes", ipr.GetLastChangeCount());
	json.Int("engine", ipr.GetEngine());
	json.Int("pass", ipr.GetPass());
	json.Int("pass_count", ipr.GetPasses().GetCount());
	json.Bool("locked", ipr.IsCameraLocked());
	json.Int("ram", memory.ram);
	json.Int("ram_total", memory.ramTotal);
	json.Int("vram", memory.vram);
	json.Int("vram_total", memory.vramTotal);
	json.Floats("mean", mean, 3);
	maxon::Float64 zoom = 0.0;
	maxon::Float64 convertMilliseconds = 0.0;
	maxon::Bool windowOpen = false;
	GetIprDialogInfo(zoom, convertMilliseconds, windowOpen);
	json.Float("latency_ms", ipr.GetLastLatencyMilliseconds());
	json.Floats("latency_parts_ms", ipr.GetLastLatencyParts(), 4);
	json.Bool("window_open", windowOpen);
	json.Float("zoom", zoom);
	json.Float("convert_ms", convertMilliseconds);
	json.EndObject();
	return json.GetText();
}

maxon::Bool RunIprTestCommand()
{
	if (!g_cyclesRenderIprTest)
		return false;

	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	const maxon::String command = stored ? stored->GetString(KEY_COMMAND) : maxon::String();
	if (command.IsEmpty())
		return false;

	cinema::BaseContainer answer;
	answer.SetString(KEY_COMMAND, maxon::String());
	answer.SetString(KEY_ANSWER, Execute(command));
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, answer, true);
	return true;
}

} // namespace cyclesrender
