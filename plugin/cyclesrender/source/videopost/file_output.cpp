// SPDX-License-Identifier: Apache-2.0
// The file Blender writes itself from a final frame (file_output.h). Reading order:
// WritesBlenderFile (whether), GetBlenderFilePath (where), WriteBlenderFileSettings and
// WriteBlenderFileField (what the bridge is told).

#include "c4d.h"
#include "c4d_videopostdata.h"
#include "lib_token.h"

#include "drendersettings.h"
#include "vpcyclesrender.h"

#include "file_output.h"
#include "render_settings_map.h"

namespace cyclesrender
{

// The switch of a pass for the file is its Picture Viewer switch plus this (tools/make_settings.py, FILE_PASS_OFFSET).
static const maxon::Int32 FILE_PASS_OFFSET = CYCLES_FILE_PASS_DEPTH - CYCLES_PASS_DEPTH;
static_assert(CYCLES_FILE_PASS_MATERIAL_INDEX - CYCLES_PASS_MATERIAL_INDEX == CYCLES_FILE_PASS_DEPTH - CYCLES_PASS_DEPTH, "the generator's offset");
static_assert(CYCLES_FILE_PASS_SHADOW_CATCHER - CYCLES_PASS_SHADOW_CATCHER == CYCLES_FILE_PASS_DEPTH - CYCLES_PASS_DEPTH, "the generator's offset");

maxon::Bool WritesBlenderFile(const cinema::BaseContainer& settings, cinema::RENDERFLAGS flags)
{
	if (!settings.GetBool(CYCLES_FILE_WRITE, false))
		return false;
	if (!(flags & cinema::RENDERFLAGS::EXTERNAL))
		return false;
	return !(flags & (cinema::RENDERFLAGS::IRR | cinema::RENDERFLAGS::PREVIEWRENDER));
}

maxon::Bool BlenderFileNeedsNames(const cinema::BaseContainer& settings)
{
	return settings.GetBool(CYCLES_FILE_WRITE, false) &&
		(settings.GetBool(CYCLES_FILE_CRYPTO_OBJECT, false) || settings.GetBool(CYCLES_FILE_CRYPTO_MATERIAL, false));
}

static maxon::Int32 CryptomatteLevels(const cinema::BaseContainer& settings)
{
	return maxon::ClampValue(settings.GetInt32(CYCLES_FILE_CRYPTO_LEVELS, 6), maxon::Int32(2), maxon::Int32(16));
}

static void SetLayerValue(JsonWriter& request, const maxon::Char* property, maxon::Bool value)
{
	request.BeginArray();
	request.String(nullptr, maxon::String(property));
	request.Bool(nullptr, value);
	request.EndArray();
}

void WriteBlenderFileSettings(JsonWriter& request, const cinema::BaseContainer& settings)
{
	request.BeginObject();
	request.String("op", "settings"_s);
	request.BeginArray("view_layer");
	for (const RenderPass& pass : GetRenderPasses())
	{
		if (!settings.GetBool(pass.id + FILE_PASS_OFFSET, false))
			continue;
		SetLayerValue(request, pass.property, true);
		if (pass.property2)
			SetLayerValue(request, pass.property2, true);
	}
	const maxon::Bool object = settings.GetBool(CYCLES_FILE_CRYPTO_OBJECT, false);
	const maxon::Bool material = settings.GetBool(CYCLES_FILE_CRYPTO_MATERIAL, false);
	if (object)
		SetLayerValue(request, "use_pass_cryptomatte_object", true);
	if (material)
		SetLayerValue(request, "use_pass_cryptomatte_material", true);
	if (object || material)
	{
		request.BeginArray();
		request.String(nullptr, "pass_cryptomatte_depth"_s);
		request.Int(nullptr, CryptomatteLevels(settings));
		request.EndArray();
	}
	request.EndArray();
}

// A path that names its drive or server, or starts at the root.
static maxon::Bool IsAbsolutePath(const maxon::String& path)
{
	if (path.GetLength() >= 2 && path[1] == ':')
		return true;
	return path.GetLength() >= 1 && (path[0] == '\\' || path[0] == '/');
}

// True when the job renders more than one frame (the Output page's Frame Range: anything but
// "Current Frame" whose first and last frame differ; Cinema 4D keeps them for every choice).
static maxon::Bool RendersSequence(const cinema::BaseContainer& renderData, maxon::Int32 fps)
{
	if (renderData.GetInt32(RDATA_FRAMESEQUENCE, RDATA_FRAMESEQUENCE_CURRENTFRAME) == RDATA_FRAMESEQUENCE_CURRENTFRAME)
		return false;
	return renderData.GetTime(RDATA_FRAMEFROM).GetFrame(fps) != renderData.GetTime(RDATA_FRAMETO).GetFrame(fps);
}

// The frame number as the Output page's Name setting writes it: Name0000.TIF, Name0000, Name.0000,
// Name000.TIF, Name000, Name.000, Name.0000.TIF (RDATA_NAMEFORMAT_0 to _6).
static maxon::String FrameSuffix(maxon::Int32 frame, maxon::Int32 nameFormat)
{
	const maxon::Bool threeDigits = nameFormat == RDATA_NAMEFORMAT_3 || nameFormat == RDATA_NAMEFORMAT_4 || nameFormat == RDATA_NAMEFORMAT_5;
	const maxon::Bool dot = nameFormat == RDATA_NAMEFORMAT_2 || nameFormat == RDATA_NAMEFORMAT_5 || nameFormat == RDATA_NAMEFORMAT_6;
	maxon::String digits = maxon::String::IntToString(maxon::Abs(frame));
	while (digits.GetLength() < (threeDigits ? 3 : 4))
		digits = "0"_s + digits;
	return (dot ? "."_s : maxon::String()) + (frame < 0 ? "-"_s : maxon::String()) + digits;
}

maxon::String GetBlenderFilePath(const cinema::BaseContainer& settings, const cinema::VideoPostStruct* vps, maxon::String& problem)
{
	problem = maxon::String();
	// A file field holds a Filename; a script may have put a plain string there.
	const cinema::GeData* const stored = settings.GetDataPointer(CYCLES_FILE_PATH);
	maxon::String raw;
	if (stored && stored->GetType() == cinema::DA_FILENAME)
		raw = stored->GetFilename().GetString();
	else if (stored && stored->GetType() == cinema::DA_STRING)
		raw = stored->GetString();
	if (raw.IsEmpty())
	{
		problem = "\"Write Files from Blender\" is on, but the Output tab names no file: nothing is written."_s;
		return maxon::String();
	}
	cinema::BaseDocument* const doc = vps->doc;
	const cinema::RenderData* const renderData = doc->GetActiveRenderData();
	const cinema::BaseContainer renderValues = vps->render->GetRenderData();
	const maxon::Int32 fps = maxon::Max(doc->GetFps(), maxon::Int32(1));
	const maxon::Int32 frame = doc->GetTime().GetFrame(fps);
	cinema::TakeData* const takes = doc->GetTakeData();
	const cinema::RenderPathData tokens(doc, renderData, &renderValues, takes ? takes->GetCurrentTake() : nullptr, frame);
	maxon::String path = cinema::StringConvertTokens(raw, &tokens);

	if (!IsAbsolutePath(path))
	{
		const cinema::Filename folder = doc->GetDocumentPath();
		if (!folder.IsPopulated())
		{
			problem = FormatString("The Output tab's file \"@\" is relative, and the document was never saved: nothing is written.", raw);
			return maxon::String();
		}
		// (Joined as text: Filename + Filename kept only the last part of "renders\still", measured.)
		maxon::String base = folder.GetString();
		if (!base.EndsWith("\\"_s) && !base.EndsWith("/"_s))
			base += "\\"_s;
		path = base + path;
	}

	// Blender's EXR is always ".exr"; the frame number goes before it.
	maxon::Int length = path.GetLength();
	if (length >= 4 && path.GetPart(length - 4, 4).ToLower() == ".exr"_s)
		path = path.GetPart(0, length - 4);
	maxon::Int position = 0;
	if (RendersSequence(renderValues, fps) && !raw.ToLower().Find("$frame"_s, &position))
		path += FrameSuffix(frame, renderValues.GetInt32(RDATA_NAMEFORMAT, RDATA_NAMEFORMAT_0));
	return path + ".exr"_s;
}

void WriteBlenderFileField(JsonWriter& request, const cinema::BaseContainer& settings, const maxon::String& path)
{
	request.BeginObject("file");
	request.String("path", path);
	const maxon::Char* const depth = GetChoiceName(settings, CYCLES_FILE_DEPTH);
	const maxon::Char* const codec = GetChoiceName(settings, CYCLES_FILE_CODEC);
	request.String("depth", maxon::String(depth ? depth : "32"));
	request.String("codec", maxon::String(codec ? codec : "ZIP"));
	request.BeginArray("passes");
	if (settings.GetBool(CYCLES_FILE_COMBINED, true))
		request.String(nullptr, "Combined"_s);
	for (const RenderPass& pass : GetRenderPasses())
	{
		if (settings.GetBool(pass.id + FILE_PASS_OFFSET, false))
			request.String(nullptr, maxon::String(pass.name));
	}
	if (settings.GetBool(CYCLES_FILE_CRYPTO_OBJECT, false))
		request.String(nullptr, "CryptoObject"_s);
	if (settings.GetBool(CYCLES_FILE_CRYPTO_MATERIAL, false))
		request.String(nullptr, "CryptoMaterial"_s);
	request.EndArray();
	request.EndObject();
}

} // namespace cyclesrender
