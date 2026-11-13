// SPDX-License-Identifier: Apache-2.0
// The IPR's test hook: commands from test scripts, and the status they get back as JSON
// (ipr_testhook.h). Only active with the development setting g_cyclesRenderIprTest.

#include "c4d.h"
#include "c4d_basematerial.h"
#include "maxon/configuration.h"
#include "maxon/derived_nodeattributes.h"
#include "maxon/file_utilities.h"
#include "maxon/nodesgraph.h"
#include "maxon/splinemapper.h"

#include "../ids.h"
#include "../materials/cycles_curves.h"
#include "../nodespace/cycles_nodes.h"
#include "../nodespace/path_commands.h"
#include "maxon/node_spaces.h"
#include "maxon/nodeslib.h"
#include "../session/json_writer.h"
#include "ipr_add.h"
#include "ipr_controller.h"
#include "ipr_dialog.h"
#include "ipr_image_area.h"
#include "ipr_testhook.h"
#include "ipr_trace.h"
#include "../materials/baked_files.h"
#include "../platform/shared_memory.h"
#include "../scene/scene_exporter.h"
#include "../materials/material_export.h"
#include "../materials/native_material.h"
#include "../prefs/devices.h"

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

// The newest frame as the IPR window shows it (DisplayTransform for the active document, without
// the window's darkened border), in the same file format with the display values 0..255 as 0..1.
static maxon::Result<void> SaveDisplay(const Frame& frame, const maxon::String& path)
{
	iferr_scope;

	DisplayTransform transform;
	transform.Update(cinema::GetActiveDocument()) iferr_return;
	Frame shown;
	shown.width = frame.width;
	shown.height = frame.height;
	shown.frameId = frame.frameId;
	const maxon::Int pixelCount = maxon::Int(frame.width) * frame.height;
	shown.pixels.Resize(pixelCount * 4) iferr_return;
	for (maxon::Int i = 0; i < pixelCount; ++i)
	{
		maxon::UChar bytes[3];
		transform.Apply(frame.pixels.GetFirst() + i * 4, bytes);
		for (maxon::Int c = 0; c < 3; ++c)
			shown.pixels[i * 4 + c] = maxon::Float32(bytes[c]) / 255.0f;
		shown.pixels[i * 4 + 3] = 1.0f;
	}
	return SaveFrame(shown, path);
}

// The newest frame as the window's "Save Image" writes it (CreateFrameBitmap, 32-bit EXR), for
// the whole frame.
static maxon::Result<void> SavePicture(const Frame& frame, const maxon::String& path)
{
	iferr_scope;

	cinema::BaseBitmap* bitmap = CreateFrameBitmap(frame, 0, 0, frame.width, frame.height, cinema::GetActiveDocument());
	if (!bitmap)
		return maxon::OutOfMemoryError(MAXON_SOURCE_LOCATION);
	const maxon::Bool saved = bitmap->Save(cinema::Filename(path), FILTER_EXR, nullptr, cinema::SAVEBIT::USE32BITCHANNELS) == cinema::IMAGERESULT::OK;
	cinema::BaseBitmap::Free(bitmap);
	if (!saved)
		return maxon::IoError(MAXON_SOURCE_LOCATION, maxon::Url(path), "the picture could not be saved"_s);
	return maxon::OK;
}

// The numbers of "a,b,c" (CurveCommand).
static maxon::Result<maxon::BaseArray<maxon::Float>> Numbers(const maxon::String& text)
{
	iferr_scope;
	maxon::BaseArray<maxon::Float> numbers;
	text.Split(","_s, true, [&numbers](const maxon::String& part) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			const maxon::Float number = part.ToFloat() iferr_return;
			numbers.Append(number) iferr_return;
			return true;
		}) iferr_return;
	return numbers;
}

// "curve <node> <port> [<knots> [<range>]]" (ipr_testhook.h): sets and reads a curve setting of a
// Cycles node, because Python cannot make a maxon::SplineMapper.
static maxon::Result<maxon::String> CurveCommand(const maxon::String& argument)
{
	iferr_scope;
	maxon::BaseArray<maxon::String> parts;
	argument.Split(" "_s, true, [&parts](const maxon::String& part) -> maxon::Result<maxon::Bool>
		{
			iferr_scope;
			if (part.IsPopulated())
			{
				parts.Append(part) iferr_return;
			}
			return true;
		}) iferr_return;
	if (parts.GetCount() < 2)
		return "{\"problem\":\"curve <node> <port> [<knots> [<range>]]\"}"_s;
	maxon::Id nodeId;
	nodeId.Init(parts[0]) iferr_return;
	maxon::Id portId;
	portId.Init(parts[1]) iferr_return;

	const maxon::Id space(CYCLES_NODESPACE_ID);
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	for (cinema::BaseMaterial* material = doc ? doc->GetFirstMaterial() : nullptr; material; material = material->GetNext())
	{
		if (!material->IsInstanceOf(Mmaterial))
			continue;
		const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
		if (!nodeMaterial->HasSpace(space))
			continue;
		const maxon::nodes::NodesGraphModelRef model = nodeMaterial->GetGraph(space) iferr_return;
		if (!model)
			continue;
		const maxon::GraphNode node = model.GetViewRoot().FindChild(nodeId) iferr_return;
		if (!node.IsValid())
			continue;
		const maxon::GraphNode inputs = node.GetInputs() iferr_return;
		const maxon::GraphNode port = inputs.FindChild(portId) iferr_return;
		if (!port.IsValid())
			continue;
		const maxon::IdAndVersion asset = node.GetValue<decltype(maxon::nodes::AssetId)>().GetOrDefault() iferr_return;
		const CyclesNode* const definition = FindCyclesNode(asset.Get<0>());
		const CyclesProperty* property = nullptr;
		for (maxon::Int i = 0; definition && i < definition->propertyCount; ++i)
		{
			if (definition->properties[i].kind == 'k' && maxon::CString(definition->properties[i].port) == portId.ToCString())
				property = &definition->properties[i];
		}
		if (!property)
			return "{\"problem\":\"that port is no curve of a Cycles node\"}"_s;
		maxon::Vector2d areaMin;
		maxon::Vector2d areaMax;
		GetCyclesCurveArea(*property, areaMin, areaMax);

		if (parts.GetCount() > 2)
		{
			// "x,y,kind[,left x,left y,right x,right y];..." with kind b (Bezier), l (linear) or c
			// (cubic), or "none"; then the range "min x,min y,max x,max y" (0,0,1,1 if not given,
			// the only one Cinema 4D's control keeps).
			maxon::SplineMapper spline = maxon::SplineMapper::Create() iferr_return;
			maxon::Vector2d min(0.0, 0.0);
			maxon::Vector2d max(1.0, 1.0);
			if (parts.GetCount() > 3)
			{
				const maxon::BaseArray<maxon::Float> range = Numbers(parts[3]) iferr_return;
				if (range.GetCount() == 4)
				{
					min = maxon::Vector2d(range[0], range[1]);
					max = maxon::Vector2d(range[2], range[3]);
				}
			}
			spline.SetRange(min, max) iferr_return;
			if (parts[2] != "none"_s)
			{
				maxon::Int index = 0;
				parts[2].Split(";"_s, true, [&spline, &index](const maxon::String& text) -> maxon::Result<maxon::Bool>
					{
						iferr_scope;
						// The third field is a letter, so the fields are read one by one.
						maxon::BaseArray<maxon::String> fields;
						text.Split(","_s, true, [&fields](const maxon::String& field) -> maxon::Result<maxon::Bool>
							{
								iferr_scope;
								fields.Append(field) iferr_return;
								return true;
							}) iferr_return;
						if (fields.GetCount() < 2)
							return true;
						maxon::SplineMapperKnot knot;
						knot.position.x = fields[0].ToFloat() iferr_return;
						knot.position.y = fields[1].ToFloat() iferr_return;
						const maxon::String kind = fields.GetCount() > 2 ? fields[2] : "b"_s;
						knot.interpolation = kind == "l"_s ? maxon::SPLINEMAPPER_KNOT_INTERPOLATION_TYPE::LINEAR
							: kind == "c"_s ? maxon::SPLINEMAPPER_KNOT_INTERPOLATION_TYPE::CUBIC : maxon::SPLINEMAPPER_KNOT_INTERPOLATION_TYPE::BEZIER;
						if (fields.GetCount() > 6)
						{
							knot.tangentLeft.x = fields[3].ToFloat() iferr_return;
							knot.tangentLeft.y = fields[4].ToFloat() iferr_return;
							knot.tangentRight.x = fields[5].ToFloat() iferr_return;
							knot.tangentRight.y = fields[6].ToFloat() iferr_return;
						}
						++index;
						knot.flags = maxon::SPLINEMAPPER_KNOT_FLAGS(index << 16);
						spline.InsertKnot(index - 1, knot) iferr_return;
						return true;
					}) iferr_return;
			}
			maxon::GraphTransaction transaction = model.BeginTransaction() iferr_return;
			port.SetPortValue(spline) iferr_return;
			transaction.Commit() iferr_return;
		}

		// What the port holds now, how Cinema 4D evaluates it, and what the exporter sends.
		const maxon::Opt<const maxon::SplineMapper&> value = port.GetEffectivePortValue<maxon::SplineMapper>() iferr_return;
		if (!value)
			return "{\"problem\":\"the port has no value\"}"_s;
		maxon::SplineMapper sorted = *value;
		sorted.SortKnots() iferr_return;
		maxon::Vector2d min;
		maxon::Vector2d max;
		sorted.GetRange(min, max);
		maxon::Float from = min.x;
		maxon::Float to = max.x;
		JsonWriter json;
		json.BeginObject();
		json.String("material", material->GetName());
		const maxon::Float64 range[4] = { min.x, min.y, max.x, max.y };
		json.Floats("range", range, 4);
		const maxon::Float64 area[4] = { areaMin.x, areaMin.y, areaMax.x, areaMax.y };
		json.Floats("area", area, 4);
		json.Float("tension", sorted.GetTension());
		json.BeginArray("knots");
		for (const maxon::SplineMapperKnot& knot : sorted.GetKnots())
		{
			// x, y, interpolation (0 Bezier, 1 linear, 2 cubic), left tangent, right tangent, flags
			const maxon::Float64 numbers[8] = { knot.position.x, knot.position.y, maxon::Float64(maxon::Int(knot.interpolation)), knot.tangentLeft.x,
				knot.tangentLeft.y, knot.tangentRight.x, knot.tangentRight.y, maxon::Float64(maxon::Int(knot.flags)) };
			json.Floats(nullptr, numbers, 8);
			from = maxon::Min(from, knot.position.x);
			to = maxon::Max(to, knot.position.x);
		}
		json.EndArray();
		// Cinema 4D's own evaluation at 1001 even positions across the span (range and knots).
		json.BeginArray("c4d");
		for (maxon::Int i = 0; i <= 1000; ++i)
		{
			const maxon::Float x = from + (to - from) * maxon::Float(i) / 1000.0;
			const maxon::Float64 point[2] = { x, sorted.GetPoint(x).y };
			json.Floats(nullptr, point, 2);
		}
		json.EndArray();
		CurvePoints sent;
		maxon::BaseArray<maxon::String> problems;
		SampleCurve(*value, areaMin, areaMax, sent, problems) iferr_return;
		json.BeginArray("sent");
		for (const maxon::Vector2d& point : sent.points)
		{
			const maxon::Float64 numbers[2] = { point.x, point.y };
			json.Floats(nullptr, numbers, 2);
		}
		json.EndArray();
		json.BeginArray("problems");
		for (const maxon::String& problem : problems)
			json.String(nullptr, problem);
		json.EndArray();
		json.EndObject();
		return json.GetText();
	}
	return "{\"problem\":\"no Cycles node of that id with that port in the materials of the active document\"}"_s;
}

// The graph of the first material of the active document in the Cycles node space.
static maxon::Result<maxon::nodes::NodesGraphModelRef> FirstCyclesGraph(maxon::String& materialName)
{
	iferr_scope;
	const maxon::Id space(CYCLES_NODESPACE_ID);
	cinema::BaseDocument* const doc = cinema::GetActiveDocument();
	for (cinema::BaseMaterial* material = doc ? doc->GetFirstMaterial() : nullptr; material; material = material->GetNext())
	{
		if (!material->IsInstanceOf(Mmaterial))
			continue;
		const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
		if (!nodeMaterial->HasSpace(space))
			continue;
		const maxon::nodes::NodesGraphModelRef model = nodeMaterial->GetGraph(space) iferr_return;
		if (model)
		{
			materialName = material->GetName();
			return model;
		}
	}
	return maxon::nodes::NodesGraphModelRef();
}

// "imagedrop <file>", "pathcommand relative|absolute <node id>", "path <node id>" (ipr_testhook.h):
// what Cinema 4D does with an image dropped on the Node Editor, as far as a script can reach it,
// and the two path buttons of the image nodes.
static maxon::Result<maxon::String> ImageNodeCommand(const maxon::String& command, const maxon::String& argument)
{
	iferr_scope;
	maxon::String materialName;
	const maxon::nodes::NodesGraphModelRef model = FirstCyclesGraph(materialName) iferr_return;
	if (!model)
		return "{\"problem\":\"the active document has no material of Cycles nodes\"}"_s;
	JsonWriter json;
	json.BeginObject();
	json.String("material", materialName);

	if (command == "imagedrop"_s)
	{
		// What the space tells Cinema 4D (NODESPACE::IMAGENODEASSETID, IMAGENODEPORTS), used as the
		// drop uses it: a node of that asset, the file in the second port. (The drop itself, a mouse
		// over the Node Editor, cannot be made by a script.)
		const maxon::DataDictionary spaceData = maxon::nodes::NodeSpaceHelpersInterface::GetNodeSpaceData(maxon::Id(CYCLES_NODESPACE_ID));
		const maxon::Id asset = spaceData.Get(maxon::nodes::NODESPACE::IMAGENODEASSETID, maxon::Id());
		const maxon::nodes::NODESPACE::ImageNodePortTuple ports = spaceData.Get(maxon::nodes::NODESPACE::IMAGENODEPORTS, maxon::nodes::NODESPACE::ImageNodePortTuple());
		json.String("asset", asset.ToString());
		const maxon::NodePath* const paths[4] = { &ports.first, &ports.second, &ports.third, &ports.fourth };
		json.BeginArray("ports");
		for (const maxon::NodePath* path : paths)
			json.String(nullptr, path->IsEmpty() ? maxon::String() : maxon::String(path->GetLastId().ToCString()));
		json.EndArray();
		if (asset.IsEmpty() || ports.second.IsEmpty())
		{
			json.String("problem", "the space names no image node"_s);
			json.EndObject();
			return json.GetText();
		}
		const maxon::nodes::NodeTemplate nodeTemplate = maxon::nodes::NodesLib::LoadTemplate(maxon::AssetInterface::GetBuiltinRepository(), asset) iferr_return;
		maxon::GraphTransaction transaction = model.BeginTransaction() iferr_return;
		const maxon::GraphNode node = model.AddChild(maxon::Id(), nodeTemplate) iferr_return;
		const maxon::GraphNode outputs = node.GetOutputs() iferr_return;
		const maxon::GraphNode inputs = node.GetInputs() iferr_return;
		const maxon::GraphNode result = outputs.FindChild(ports.first.GetLastId()) iferr_return;
		const maxon::GraphNode file = inputs.FindChild(ports.second.GetLastId()) iferr_return;
		const maxon::GraphNode first = inputs.FindChild(ports.third.GetLastId()) iferr_return;
		const maxon::GraphNode last = inputs.FindChild(ports.fourth.GetLastId()) iferr_return;
		json.Bool("ports_found", result.IsValid() && file.IsValid() && first.IsValid() && last.IsValid());
		if (file.IsValid())
		{
			maxon::Url url;
			url.SetSystemPath(argument) iferr_return;
			file.SetPortValue(url) iferr_return;
		}
		transaction.Commit() iferr_return;
		json.String("node", maxon::String(node.GetId().ToCString()));
	}
	else
	{
		maxon::String nodeName = argument;
		maxon::Bool relative = true;
		if (command == "pathcommand"_s)
		{
			maxon::Int space = 0;
			if (!argument.Find(" "_s, &space))
				return "{\"problem\":\"pathcommand relative|absolute <node id>\"}"_s;
			relative = argument.GetLeftPart(space) == "relative"_s;
			nodeName = argument.GetRightPart(space + 1);
		}
		maxon::Id nodeId;
		nodeId.Init(nodeName) iferr_return;
		const maxon::GraphNode node = model.GetViewRoot().FindChild(nodeId) iferr_return;
		if (!node.IsValid())
			return "{\"problem\":\"no node of that id\"}"_s;
		if (command == "pathcommand"_s)
		{
			const maxon::String result = InvokePathCommand(relative, model, node) iferr_return;
			json.String("result", result);
		}
		const maxon::GraphNode inputs = node.GetInputs() iferr_return;
		const maxon::GraphNode file = inputs.FindChild(maxon::Id("prop.image_file")) iferr_return;
		maxon::Url url;
		if (file.IsValid())
		{
			url = file.GetEffectivePortValue<maxon::Url>().GetValueOr(maxon::Url()) iferr_return;
		}
		json.String("scheme", url.GetScheme().ToString());
		json.String("path", url.GetPath());
	}
	json.EndObject();
	return json.GetText();
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
	else if (command == "frame"_s || command == "display"_s || command == "picture"_s)
	{
		if (frame.frameId == 0)
		{
			problem = "no frame yet"_s;
		}
		else if (command == "frame"_s)
		{
			iferr (SaveFrame(frame, argument))
				problem = err.GetMessage();
		}
		else if (command == "display"_s)
		{
			iferr (SaveDisplay(frame, argument))
				problem = err.GetMessage();
		}
		else
		{
			iferr (SavePicture(frame, argument))
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
	else if (command == "verify"_s)
	{
		// The active document exported whole against what Blender holds (IprController::VerifySync).
		iferr (const maxon::String answer = ipr.VerifySync())
			return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		return answer;
	}
	else if (command == "sources"_s)
	{
		// What the mesh leaves of the active document were made from (SceneExporter::GetSourceListing).
		cinema::BaseDocument* const doc = cinema::GetActiveDocument();
		if (!doc)
			return "{\"problem\":\"no document\"}"_s;
		SceneExporter exporter(doc, ExportSettings());
		iferr (exporter.Collect())
			return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		iferr (const maxon::String listing = exporter.GetSourceListing())
			return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		return listing;
	}
	else if (command == "trace"_s)
	{
		// "trace on" / "trace off" switch the timeline (ipr_trace.h) and forget it; "trace" hands it
		// over and forgets it.
		if (argument == "on"_s || argument == "off"_s)
		{
			SetIprTracing(argument == "on"_s);
			return "{\"trace\":[]}"_s;
		}
		return FormatString("{\"trace\":@,\"now\":@}", TakeIprTrace(), maxon::String::FloatToString(platform::UnixTime(), -1, 6));
	}
	else if (command == "curve"_s)
	{
		iferr (const maxon::String answer = CurveCommand(argument))
			return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		return answer;
	}
	else if (command == "add"_s)
	{
		// What the window's "Add" menu does, without the window: material, camera, camera_tag,
		// object_tag, light, light_tag. "add follow" runs what the message plugin runs after a change
		// (c4dpy has no message loop): the render view through undo and redo of "add camera".
		if (argument == "follow"_s)
		{
			const maxon::Int set = FollowAddedCameras();
			return FormatString("{\"views_set\":@,\"switches\":@}", set, GetAddedCameraSwitchCount());
		}
		const IPRADD what = argument == "material"_s ? IPRADD::CYCLES_MATERIAL : argument == "camera"_s ? IPRADD::CAMERA_WITH_TAG
			: argument == "camera_tag"_s ? IPRADD::CAMERA_TAG : argument == "light"_s ? IPRADD::CYCLES_LIGHT
			: argument == "light_tag"_s ? IPRADD::LIGHT_TAG : IPRADD::OBJECT_TAG;
		maxon::String message;
		const maxon::Int added = AddToDocument(cinema::GetActiveDocument(), what, message);
		JsonWriter json;
		json.BeginObject();
		json.Int("added", added);
		json.String("message", message);
		json.EndObject();
		return json.GetText();
	}
	else if (command == "imagedrop"_s || command == "pathcommand"_s || command == "path"_s)
	{
		iferr (const maxon::String answer = ImageNodeCommand(command, argument))
			return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		return answer;
	}
	else if (command == "devices"_s)
	{
		// The Preferences page's Cycles Render Devices (prefs/devices.h): "find" asks a Blender as the
		// button does, "type <n>" chooses a type, "use <index> <0|1>" ticks a checkbox of the page,
		// "reset" forgets everything; then the settings as JSON.
		if (argument == "find"_s)
		{
			iferr (FindDevices())
				return FormatString("{\"problem\":\"@\"}", err.GetMessage());
		}
		else if (argument.StartsWith("type "_s))
		{
			iferr (const maxon::Int type = argument.GetPart(5, argument.GetLength() - 5).ToInt())
				return "{\"problem\":\"no type\"}"_s;
			SetDeviceType(DEVICETYPE(maxon::Int32(type)));
		}
		else if (argument.StartsWith("use "_s))
		{
			maxon::BaseArray<maxon::String> parts;
			iferr (argument.Split(" "_s, true, parts))
				return "{\"problem\":\"no memory\"}"_s;
			iferr (const DeviceSettings settings = GetDeviceSettings())
				return "{\"problem\":\"no settings\"}"_s;
			iferr (const maxon::BaseArray<CyclesDevice> boxes = DevicesOfType(settings, settings.type))
				return "{\"problem\":\"no list\"}"_s;
			const maxon::Int index = parts.GetCount() == 3 ? parts[1].ToInt().GetValueOr(-1) : -1;
			if (index < 0 || index >= boxes.GetCount())
				return "{\"problem\":\"no such checkbox\"}"_s;
			SetDeviceUse(boxes[index].id, parts[2] == "1"_s);
		}
		else if (argument == "reset"_s)
		{
			ResetDeviceSettings();
		}
		return DescribeDeviceSettings();
	}
	else if (command == "bakes"_s)
	{
		// The baked files instead of the status; "bakes check" first compares the documents with
		// the document list (as the message hook does), "bakes cleanup" first runs the clean-up of
		// earlier sessions that runs at start-up.
		if (argument == "check"_s)
			CheckOpenDocuments();
		else if (argument == "cleanup"_s)
			RemoveBakedFilesOfEarlierSessions();
		const maxon::String files = DescribeBakedFiles();
		return FormatString("{\"bakes\":@,\"store\":@}", MaterialExporter::GetBakeCount(), files);
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
	json.Int("meshes_exported", ipr.GetLastExportStats().meshesExported);
	json.Int("meshes_kept", ipr.GetLastExportStats().meshesKept);
	json.Int("meshes_by_identity", ipr.GetLastExportStats().meshesByIdentity);
	json.Float("export_ms", ipr.GetLastExportMilliseconds());
	json.Float("mesh_ms", ipr.GetLastExportStats().meshMilliseconds);
	json.Int("engine", ipr.GetEngine());
	json.Int("pass", ipr.GetPass());
	json.Int("pass_count", ipr.GetPasses().GetCount());
	json.Bool("locked", ipr.IsCameraLocked());
	json.Int("ram", memory.ram);
	json.Int("ram_total", memory.ramTotal);
	json.Int("vram", memory.vram);
	json.Int("vram_total", memory.vramTotal);
	json.Floats("mean", mean, 3);
	json.Int("bakes", MaterialExporter::GetBakeCount());
	maxon::Float64 zoom = 0.0;
	maxon::Float64 zoomField = 0.0;
	maxon::Float64 convertMilliseconds = 0.0;
	maxon::Bool windowOpen = false;
	GetIprDialogInfo(zoom, zoomField, convertMilliseconds, windowOpen);
	json.Float("latency_ms", ipr.GetLastLatencyMilliseconds());
	json.Floats("latency_parts_ms", ipr.GetLastLatencyParts(), 4);
	json.Bool("window_open", windowOpen);
	json.Float("zoom", zoom);
	json.Float("zoom_field", zoomField);
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
