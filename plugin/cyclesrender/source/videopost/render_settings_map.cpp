// SPDX-License-Identifier: Apache-2.0
// The Render Settings as a `settings` request, and what the page shows for which engine
// (render_settings_map.h). The settings themselves are a generated table:
// render_settings_table.h, written by tools/make_settings.py.

#include "c4d.h"
#include "ddoc.h"

#include "vpcyclesrender.h"

#include "../ids.h"
#include "../scene/document_colors.h"
#include "../session/capture.h"
#include "../session/json.h"
#include "render_settings_map.h"

namespace cyclesrender
{

enum class SETTINGTYPE
{
	INT,
	FLOAT,
	BOOL
} MAXON_ENUM_LIST(SETTINGTYPE);

struct Setting
{
	maxon::Int32 id;          // parameter id in vpcyclesrender.h
	SETTINGTYPE type;
	maxon::Float64 fallback;  // default of a new renderer; also used when a scene file predates the setting
	const maxon::Char* path;  // Blender property, relative to the scene; nullptr when not sent as a property
	const maxon::Char* path2; // a second property that takes the same value, or nullptr
};

// Settings that choose one of several named options. The value stored in the scene file is the
// position in `options`; Blender receives the option's name. Where Blender does not offer an
// option on this machine (OptiX without an RTX card) the bridge keeps its own choice and warns.
struct ChoiceSetting
{
	maxon::Int32 id;
	maxon::Int32 fallback;
	const maxon::Char* path;
	const maxon::Char* path2;
	const maxon::Char* options[12];  // ends with nullptr; the table says how many it uses at most (CHOICE_OPTIONS)
};

struct EngineOnly
{
	maxon::Int32 id;      // a group or a setting
	maxon::Int32 engine;  // CYCLES_ENGINE_CYCLES or CYCLES_ENGINE_EEVEE
};

struct EnabledBy
{
	maxon::Int32 id;      // the setting that is greyed out ...
	maxon::Int32 other;   // ... unless this one ...
	maxon::Int32 value;   // ... has this value (1 for a ticked checkbox)
};

// The tables SETTINGS, CHOICES, ENGINE_ONLY and ENABLED_BY. Every setting of the renderer is
// defined once, in tools/make_settings.py, which writes this table together with the ids, the
// layout and the texts of the page. To add a setting: add it there and run the script.
// Defaults are Blender's own.
#include "render_settings_table.h"

static const maxon::Char* GetChoice(const cinema::BaseContainer& data, const ChoiceSetting& choice)
{
	maxon::Int32 index = data.FindIndex(choice.id) == NOTOK ? choice.fallback : data.GetInt32(choice.id);
	maxon::Int32 count = 0;
	while (count < CHOICE_OPTIONS && choice.options[count])
		++count;
	if (index < 0 || index >= count)
		index = choice.fallback;
	return choice.options[index];
}

// The render passes a final frame can deliver next to the picture. The name is Blender's own
// name of the pass, and the name of the layer in the Picture Viewer; the property switches
// the pass on, relative to Blender's view layer.
static const RenderPass PASSES[] =
{
	{ CYCLES_PASS_DEPTH,                  "Depth", "use_pass_z" },
	{ CYCLES_PASS_MIST,                   "Mist", "use_pass_mist" },
	{ CYCLES_PASS_POSITION,               "Position", "use_pass_position" },
	{ CYCLES_PASS_NORMAL,                 "Normal", "use_pass_normal" },
	{ CYCLES_PASS_UV,                     "UV", "use_pass_uv" },
	{ CYCLES_PASS_DIFFUSE_DIRECT,         "Diffuse Direct", "use_pass_diffuse_direct" },
	{ CYCLES_PASS_DIFFUSE_INDIRECT,       "Diffuse Indirect", "use_pass_diffuse_indirect" },
	{ CYCLES_PASS_DIFFUSE_COLOR,          "Diffuse Color", "use_pass_diffuse_color" },
	{ CYCLES_PASS_GLOSSY_DIRECT,          "Glossy Direct", "use_pass_glossy_direct" },
	{ CYCLES_PASS_GLOSSY_INDIRECT,        "Glossy Indirect", "use_pass_glossy_indirect" },
	{ CYCLES_PASS_GLOSSY_COLOR,           "Glossy Color", "use_pass_glossy_color" },
	{ CYCLES_PASS_TRANSMISSION_DIRECT,    "Transmission Direct", "use_pass_transmission_direct" },
	{ CYCLES_PASS_TRANSMISSION_INDIRECT,  "Transmission Indirect", "use_pass_transmission_indirect" },
	{ CYCLES_PASS_TRANSMISSION_COLOR,     "Transmission Color", "use_pass_transmission_color" },
	{ CYCLES_PASS_EMISSION,               "Emission", "use_pass_emit" },
	{ CYCLES_PASS_ENVIRONMENT,            "Environment", "use_pass_environment" },
	{ CYCLES_PASS_AO,                     "Ambient Occlusion", "use_pass_ambient_occlusion" },
};

maxon::Block<const RenderPass> GetRenderPasses()
{
	return maxon::Block<const RenderPass>(PASSES, SIZEOF(PASSES) / SIZEOF(PASSES[0]));
}

static const cinema::Vector DEFAULT_WORLD_COLOR = cinema::Vector(0.05);

static const Setting* FindSetting(maxon::Int32 id)
{
	for (const Setting& setting : SETTINGS)
	{
		if (setting.id == id)
			return &setting;
	}
	return nullptr;
}

// The value of a setting, falling back to its default when the container lacks it.
static maxon::Float64 GetValue(const cinema::BaseContainer& data, const Setting& setting)
{
	if (data.FindIndex(setting.id) == NOTOK)
		return setting.fallback;
	switch (setting.type)
	{
		case SETTINGTYPE::INT: return maxon::Float64(data.GetInt32(setting.id));
		case SETTINGTYPE::BOOL: return data.GetBool(setting.id) ? 1.0 : 0.0;
		default: return data.GetFloat(setting.id);
	}
}

static maxon::Float64 GetValue(const cinema::BaseContainer& data, maxon::Int32 id)
{
	const Setting* const setting = FindSetting(id);
	return setting ? GetValue(data, *setting) : 0.0;
}

void InitRenderSettings(cinema::BaseContainer& data)
{
	data.SetInt32(CYCLES_ENGINE, CYCLES_ENGINE_CYCLES);
	data.SetInt32(CYCLES_DEVICE, CYCLES_DEVICE_CPU);	// Blender's own default
	data.SetInt32(CYCLES_LAYOUT, CYCLES_LAYOUT_SIMPLE);
	data.SetVector(CYCLES_WORLD_COLOR, DEFAULT_WORLD_COLOR);
	for (const Setting& setting : SETTINGS)
	{
		switch (setting.type)
		{
			case SETTINGTYPE::INT: data.SetInt32(setting.id, maxon::Int32(setting.fallback)); break;
			case SETTINGTYPE::BOOL: data.SetBool(setting.id, setting.fallback != 0.0); break;
			default: data.SetFloat(setting.id, setting.fallback); break;
		}
	}
	for (const RenderPass& pass : PASSES)
		data.SetBool(pass.id, false);
	for (const ChoiceSetting& choice : CHOICES)
		data.SetInt32(choice.id, choice.fallback);
}

// Blender's name for the colour space the document renders in, or an empty string (plus a
// warning) when Blender has no working space to match.
static maxon::String BlenderWorkingSpace(const cinema::BaseDocument* doc, maxon::BaseArray<maxon::String>& warnings)
{
	// "Basic" colour management calculates in linear sRGB, or without Linear Workflow on the
	// sRGB numbers themselves, which Blender is given as if they were linear (DocumentColors).
	if (!DocumentColors(doc).IsOcio())
		return "Linear Rec.709"_s;

	maxon::CString render;
	maxon::CString display;
	maxon::CString view;
	maxon::CString thumbnails;
	doc->GetActiveOcioColorSpacesNames(render, display, view, thumbnails);
	const maxon::String name(render);
	maxon::Int position = 0;
	if (name.Find("ACEScg"_s, &position))
		return "ACEScg"_s;
	if (name.Find("Rec.709"_s, &position) || name.Find("sRGB"_s, &position))
		return "Linear Rec.709"_s;
	if (name.Find("Rec.2020"_s, &position))
		return "Linear Rec.2020"_s;
	warnings.Append(FormatString("The document renders in the colour space \"@\", which Blender cannot use as its working space. "
		"Colours and textures may look different.", name)) iferr_ignore("A lost warning is not worth failing the render.");
	return maxon::String();
}

void HideSettingsOfOtherEngine(const cinema::BaseContainer& data, cinema::Description* description)
{
	const maxon::Int32 engine = data.GetInt32(CYCLES_ENGINE, CYCLES_ENGINE_CYCLES);
	// ... and, in the Simple layout, without what only the Advanced layout has.
	const maxon::Bool simple = data.GetInt32(CYCLES_LAYOUT, CYCLES_LAYOUT_SIMPLE) == CYCLES_LAYOUT_SIMPLE;
	for (const maxon::Int32 id : ADVANCED_ONLY)
	{
		if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(id)), nullptr))
			parameter->SetBool(cinema::DESC_HIDE, simple);
	}
	for (const EngineOnly& entry : ENGINE_ONLY)
	{
		if (entry.engine == engine)
			continue;   // shown, unless the layout hid it above
		if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(entry.id)), nullptr))
			parameter->SetBool(cinema::DESC_HIDE, true);
	}
	// The device is a setting of Cycles.
	if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(CYCLES_DEVICE)), nullptr))
		parameter->SetBool(cinema::DESC_HIDE, engine != CYCLES_ENGINE_CYCLES);
}

maxon::Bool IsSettingEnabled(const cinema::BaseContainer& data, maxon::Int32 id)
{
	for (const EnabledBy& entry : ENABLED_BY)
	{
		if (entry.id != id)
			continue;
		maxon::Int32 value = 0;
		if (const Setting* const setting = FindSetting(entry.other))
		{
			value = maxon::Int32(GetValue(data, *setting));
		}
		else
		{
			for (const ChoiceSetting& choice : CHOICES)
			{
				if (choice.id == entry.other)
					value = data.FindIndex(choice.id) == NOTOK ? choice.fallback : data.GetInt32(choice.id);
			}
		}
		if (value != entry.value)
			return false;
	}
	return true;
}

cinema::BaseContainer GetDocumentRenderSettings(cinema::BaseDocument* doc)
{
	cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
	for (cinema::BaseVideoPost* post = renderData ? renderData->GetFirstVideoPost() : nullptr; post; post = post->GetNext())
	{
		if (post->GetType() == ID_CYCLES_VIDEOPOST)
			return post->GetDataInstanceRef();
	}
	cinema::BaseContainer defaults;
	InitRenderSettings(defaults);
	return defaults;
}

maxon::Int32 WriteRenderSettings(JsonWriter& request, const cinema::BaseContainer& data, const cinema::BaseDocument* doc,
	maxon::Int32 engineOverride, maxon::BaseArray<maxon::String>& warnings)
{
	const maxon::Int32 engine = engineOverride == CYCLES_ENGINE_CYCLES || engineOverride == CYCLES_ENGINE_EEVEE ?
		engineOverride : data.GetInt32(CYCLES_ENGINE, CYCLES_ENGINE_CYCLES);

	request.BeginObject();
	request.String("op", "settings"_s);
	request.String("engine", engine == CYCLES_ENGINE_EEVEE ? "EEVEE"_s : "CYCLES"_s);
	request.String("device", data.GetInt32(CYCLES_DEVICE, CYCLES_DEVICE_CPU) == CYCLES_DEVICE_GPU ? "GPU"_s : "CPU"_s);
	const maxon::String workingSpace = doc ? BlenderWorkingSpace(doc, warnings) : maxon::String();
	if (!workingSpace.IsEmpty())
		request.String("working_space", workingSpace);

	request.BeginArray("values");
	for (const Setting& setting : SETTINGS)
	{
		const maxon::Float64 value = GetValue(data, setting);
		const maxon::Char* const paths[2] = { setting.path, setting.path2 };
		for (const maxon::Char* path : paths)
		{
			if (!path)
				continue;
			request.BeginArray();
			request.String(nullptr, maxon::String(path));
			switch (setting.type)
			{
				case SETTINGTYPE::INT: request.Int(nullptr, maxon::Int64(value)); break;
				case SETTINGTYPE::BOOL: request.Bool(nullptr, value != 0.0); break;
				default: request.Float(nullptr, value); break;
			}
			request.EndArray();
		}
	}
	for (const ChoiceSetting& choice : CHOICES)
	{
		const maxon::Char* const paths[2] = { choice.path, choice.path2 };
		for (const maxon::Char* path : paths)
		{
			if (!path)
				continue;
			request.BeginArray();
			request.String(nullptr, maxon::String(path));
			request.String(nullptr, maxon::String(GetChoice(data, choice)));
			request.EndArray();
		}
	}
	request.EndArray();

	request.BeginArray("view_layer");
	for (const RenderPass& pass : PASSES)
	{
		request.BeginArray();
		request.String(nullptr, maxon::String(pass.property));
		request.Bool(nullptr, data.GetBool(pass.id, false));
		request.EndArray();
	}
	request.EndArray();
	return engine;
}

maxon::Result<void> SendRenderSettings(BridgeClient& client, const cinema::BaseContainer& data, const cinema::BaseDocument* doc,
	cinema::BaseThread* cancel, maxon::BaseArray<maxon::String>& warnings)
{
	iferr_scope;

	JsonWriter request;
	WriteRenderSettings(request, data, doc, NOTOK, warnings);
	const BridgeMessage reply = client.Request(request, BlobViews(), cancel) iferr_return;
	for (const maxon::String& warning : GetJsonStrings(reply.header, "warnings"_s))
	{
		warnings.Append(warning) iferr_return;
	}

	// For tests: ask Blender what it holds now, so the capture file shows the settings arrived.
	if (IsCapturing())
	{
		JsonWriter query;
		query.BeginObject();
		query.String("op", "get"_s);
		query.BeginArray("paths");
		for (const Setting& setting : SETTINGS)
		{
			if (setting.path)
				query.String(nullptr, maxon::String(setting.path));
			if (setting.path2)
				query.String(nullptr, maxon::String(setting.path2));
		}
		for (const ChoiceSetting& choice : CHOICES)
		{
			if (choice.path)   // a choice the plugin reads itself has none
				query.String(nullptr, maxon::String(choice.path));
			if (choice.path2)
				query.String(nullptr, maxon::String(choice.path2));
		}
		query.EndArray();
		client.Request(query, BlobViews(), cancel) iferr_return;
	}
	return maxon::OK;
}

ExportSettings GetExportSettings(const cinema::BaseContainer& data, maxon::Int32 width, maxon::Int32 height)
{
	ExportSettings settings;
	settings.width = width;
	settings.height = height;
	settings.depthOfField = GetValue(data, CYCLES_DEPTH_OF_FIELD) != 0.0;
	settings.lightMultiplier = GetValue(data, CYCLES_LIGHT_MULTIPLIER);
	settings.worldColor = data.FindIndex(CYCLES_WORLD_COLOR) == NOTOK ? DEFAULT_WORLD_COLOR : data.GetVector(CYCLES_WORLD_COLOR);
	settings.worldStrength = GetValue(data, CYCLES_WORLD_STRENGTH);
	settings.motionBlur = GetValue(data, CYCLES_MOTION_BLUR) != 0.0;
	settings.shutter = maxon::ClampValue(GetValue(data, CYCLES_MOTION_SHUTTER), 0.0, 2.0);
	// Where the open shutter lies relative to the frame: it starts there, is centred on it, or ends there.
	const maxon::Int32 position = data.FindIndex(CYCLES_MOTION_POSITION) == NOTOK ? maxon::Int32(CYCLES_MOTION_POSITION_CENTER) : data.GetInt32(CYCLES_MOTION_POSITION);
	settings.shutterCentre = position == CYCLES_MOTION_POSITION_START ? settings.shutter * 0.5 : position == CYCLES_MOTION_POSITION_END ? -settings.shutter * 0.5 : 0.0;
	settings.bakeResolution = maxon::ClampValue(maxon::Int32(GetValue(data, CYCLES_BAKE_RESOLUTION)), maxon::Int32(16), maxon::Int32(8192));
	return settings;
}

} // namespace cyclesrender
