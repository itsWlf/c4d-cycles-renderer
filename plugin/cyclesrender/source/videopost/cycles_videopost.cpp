// SPDX-License-Identifier: Apache-2.0
// The renderer: what Cinema 4D calls to render a frame (cycles_videopost.h).
//
// Final frames in short: Execute() is called for every stage of every frame. When the render
// stage closes, RenderFrame() exports the scene, sends what changed since the frame before,
// asks Blender for a render, waits for the pixels while watching for the user's cancel, and
// writes the picture and the render passes into Cinema 4D's buffers. One Blender serves the
// whole render job (OpenSession, CloseSession).

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "c4d_videopost.h"
#include "maxon/configuration.h"

#include "ddoc.h"
#include "drendersettings.h"
#include "vpcyclesrender.h"

#include "../ids.h"
#include "../scene/document_colors.h"
#include "../scene/scene_exporter.h"
#include "../scene/scene_writer.h"
#include "../session/capture.h"
#include "../session/json.h"
#include "cycles_videopost.h"
#include "output_settings.h"
#include "render_settings_map.h"

// Start Cinema 4D with g_cyclesRenderLogCalls=true to see every call the renderer receives.
MAXON_CONFIGURATION_BOOL(g_cyclesRenderLogCalls, false, maxon::CONFIGURATION_CATEGORY::DEVELOPMENT,
	"Cycles Render: write every renderer call (stage, open/close, buffer format) and the stages of each frame to the console.");

namespace cyclesrender
{

cinema::Bool CyclesVideoPost::Init(cinema::GeListNode* node, cinema::Bool isCloneInit)
{
	cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
	if (!item)
		return false;

	if (!isCloneInit)
	{
		InitRenderSettings(item->GetDataInstanceRef());
		// The name of the entry in the Render Settings' list; the renderer itself is "Cycles Render".
		item->SetName("Cycles"_s);
	}
	return true;
}

void CyclesVideoPost::Free(cinema::GeListNode* node)
{
	// A render job that never reached its closing call must not leave Blender behind.
	CloseSession(false);
}

void CyclesVideoPost::AllocateBuffers(cinema::BaseVideoPost* node, cinema::Render* render, cinema::BaseDocument* doc)
{
	_passes.Reset();
	if (!node || !render)
		return;
	const cinema::BaseContainer& data = node->GetDataInstanceRef();
	for (const RenderPass& pass : GetRenderPasses())
	{
		if (!data.GetBool(pass.id, false))
			continue;
		PassBuffer buffer;
		buffer.pass = &pass;
		buffer.bufferId = render->AllocateBufferFX(VPBUFFER_POSTEFFECT, maxon::String(pass.name), 32, true);
		_passes.Append(buffer) iferr_ignore("A pass less is acceptable when memory is out.");
	}
}

// What Cinema 4D expects in the picture's buffer; measured against the Standard renderer with
// a colour chart (tests\c4d\colour.py).
enum class OUTPUTCOLORS
{
	RENDER_SPACE,   ///< the values as rendered: OCIO documents, where Cinema 4D shows and saves them through the view transform itself, and "Basic" ones without the curve
	VIEW,           ///< OCIO documents asked to bake the view transform into the picture (RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER)
	SRGB_CURVE      ///< "Basic" colour management with Linear Workflow: the picture is stored with the sRGB curve applied (DocumentColors::CurveInPicture)
} MAXON_ENUM_LIST(OUTPUTCOLORS);

static OUTPUTCOLORS GetOutputColors(const cinema::BaseDocument* doc, const cinema::BaseContainer& renderData)
{
	const DocumentColors colors(doc);
	if (colors.IsOcio())
		return renderData.GetBool(RDATA_BAKE_OCIO_VIEW_TRANSFORM_RENDER, true) ? OUTPUTCOLORS::VIEW : OUTPUTCOLORS::RENDER_SPACE;
	// "Basic": Blender calculated with the numbers Cinema 4D calculates with, linear with
	// Linear Workflow and the sRGB numbers without; the buffer wants the curve on the linear
	// ones, unless Multi-Pass is on (measured: docs/reference/c4d-sdk.md, "Colour management").
	return colors.CurveInPicture(renderData) ? OUTPUTCOLORS::SRGB_CURVE : OUTPUTCOLORS::RENDER_SPACE;
}

// Writes RGBA float pixels, top row first, into one of Cinema 4D's buffers.
// @param[in] converter  For OUTPUTCOLORS::VIEW: the document's OCIO converter.
static maxon::Result<void> FillBuffer(cinema::VPBuffer* buffer, const Blob& pixels, maxon::Int32 width, maxon::Int32 height,
	OUTPUTCOLORS colors = OUTPUTCOLORS::RENDER_SPACE, cinema::OcioConverter* converter = nullptr)
{
	iferr_scope;

	const maxon::Int channels = 4;
	const maxon::Int rowValues = maxon::Int(width) * channels;
	if (pixels.GetCount() != rowValues * height * SIZEOF(maxon::Float32))
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Blender's pixel data has the wrong length."_s);

	const maxon::Float32* const source = reinterpret_cast<const maxon::Float32*>(pixels.GetFirst());
	const maxon::Int32 targetChannels = buffer->GetCpp();
	maxon::BaseArray<maxon::Float32> line;
	line.Resize(maxon::Int(width) * targetChannels) iferr_return;
	maxon::BaseArray<cinema::Vector32> converted;
	if (colors == OUTPUTCOLORS::VIEW && converter && targetChannels >= 3)
	{
		converted.Resize(width) iferr_return;
	}
	for (maxon::Int32 y = 0; y < height; ++y)
	{
		const maxon::Float32* const row = source + maxon::Int(y) * rowValues;
		for (maxon::Int32 x = 0; x < width; ++x)
		{
			for (maxon::Int32 c = 0; c < targetChannels; ++c)
				line[maxon::Int(x) * targetChannels + c] = c < channels ? row[maxon::Int(x) * channels + c] : 1.0f;
		}
		if (!converted.IsEmpty())
		{
			for (maxon::Int32 x = 0; x < width; ++x)
				converted[x] = cinema::Vector32(row[maxon::Int(x) * channels], row[maxon::Int(x) * channels + 1], row[maxon::Int(x) * channels + 2]);
			converter->TransformColors(converted.GetFirst(), width, cinema::COLORSPACETRANSFORMATION::OCIO_RENDERING_TO_VIEW);
			for (maxon::Int32 x = 0; x < width; ++x)
			{
				line[maxon::Int(x) * targetChannels + 0] = converted[x].x;
				line[maxon::Int(x) * targetChannels + 1] = converted[x].y;
				line[maxon::Int(x) * targetChannels + 2] = converted[x].z;
			}
		}
		else if (colors == OUTPUTCOLORS::SRGB_CURVE && targetChannels >= 3)
		{
			for (maxon::Int32 x = 0; x < width; ++x)
			{
				for (maxon::Int32 c = 0; c < 3; ++c)
				{
					maxon::Float32& value = line[maxon::Int(x) * targetChannels + c];
					value = LinearToSrgb(value);
				}
			}
		}
		buffer->SetLine(0, y, width, line.GetFirst(), 32, false);
	}
	return maxon::OK;
}

cinema::VIDEOPOSTINFO CyclesVideoPost::GetRenderInfo(cinema::BaseVideoPost* node)
{
	// The plugin reads the scene itself, so Cinema 4D can skip preparing it for its own renderer.
	return cinema::VIDEOPOSTINFO::EVALUATES_SCENE;
}

cinema::Bool CyclesVideoPost::GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const
{
	if (!node || !description || !description->LoadDescription(node->GetType()))
		return false;
	flags |= cinema::DESCFLAGS_DESC::LOADED;

	// One page serves both engines: show the groups of the chosen one.
	HideSettingsOfOtherEngine(static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef(), description);
	return SUPER::GetDDescription(node, description, flags);
}

cinema::Bool CyclesVideoPost::GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
	cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const
{
	if (node && id.GetDepth() > 0 && !IsSettingEnabled(static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef(), id[0].id))
		return false;
	return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
}

// A render pass reaches the Picture Viewer only when Multi-Pass is on and has a "Post Effects"
// entry (AllocateBuffers). Switching a pass on sets both up, as other render plugins do.
static void PrepareMultiPass(cinema::GeListNode* node)
{
	cinema::BaseDocument* const doc = node ? node->GetDocument() : nullptr;
	if (!doc)
		return;
	for (cinema::RenderData* renderData = doc->GetFirstRenderData(); renderData; )
	{
		for (cinema::BaseVideoPost* post = renderData->GetFirstVideoPost(); post; post = post->GetNext())
		{
			if (post != node)
				continue;
			maxon::Bool found = false;
			for (cinema::MultipassObject* pass = renderData->GetFirstMultipass(); pass && !found; pass = pass->GetNext())
			{
				const maxon::Int32 type = pass->GetDataInstanceRef().GetInt32(MULTIPASSOBJECT_TYPE);
				found = type == VPBUFFER_ALLPOSTEFFECTS || type == VPBUFFER_POSTEFFECT;
			}
			if (!found)
			{
				cinema::MultipassObject* const pass = static_cast<cinema::MultipassObject*>(cinema::MultipassObject::Alloc(Zmultipass));
				if (pass)
				{
					pass->GetDataInstanceRef().SetInt32(MULTIPASSOBJECT_TYPE, VPBUFFER_ALLPOSTEFFECTS);
					renderData->InsertMultipass(pass);
				}
			}
			renderData->GetDataInstanceRef().SetBool(RDATA_MULTIPASS_ENABLE, true);
			return;
		}
		// Render settings can be nested: children first, then the next one, then back up.
		if (renderData->GetDown())
		{
			renderData = renderData->GetDown();
		}
		else
		{
			while (renderData && !renderData->GetNext())
				renderData = renderData->GetUp();
			if (renderData)
				renderData = renderData->GetNext();
		}
	}
}

cinema::Bool CyclesVideoPost::GetDParameter(const cinema::GeListNode* node, const cinema::DescID& id, cinema::GeData& data, cinema::DESCFLAGS_GET& flags) const
{
	if (node && id.GetDepth() > 0 && GetOutputParameter(static_cast<const cinema::BaseList2D*>(node), id[0].id, data))
	{
		flags |= cinema::DESCFLAGS_GET::PARAM_GET;
		return true;
	}
	return SUPER::GetDParameter(node, id, data, flags);
}

cinema::Bool CyclesVideoPost::SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data, cinema::DESCFLAGS_SET& flags)
{
	// The Output tab writes Cinema 4D's own Width, Height and Pixel Aspect too (output_settings.h).
	if (node && id.GetDepth() > 0 && SetOutputParameter(static_cast<cinema::BaseList2D*>(node), id[0].id, data))
	{
		flags |= cinema::DESCFLAGS_SET::PARAM_SET;
		return true;
	}
	if (id.GetDepth() > 0 && data.GetType() == cinema::DA_LONG && data.GetInt32() != 0)
	{
		for (const RenderPass& pass : GetRenderPasses())
		{
			if (pass.id == id[0].id)
			{
				PrepareMultiPass(node);
				break;
			}
		}
	}
	return SUPER::SetDParameter(node, id, data, flags);
}

cinema::Bool CyclesVideoPost::RenderEngineCheck(const cinema::BaseVideoPost* node, cinema::Int32 id) const
{
	// Hide the Render Settings tabs that only apply to Cinema 4D's own renderers.
	switch (id)
	{
		case RENDERSETTING_STATICTAB_ANTIALIASING:
		case RENDERSETTING_STATICTAB_OPTIONS:
		case RENDERSETTING_STATICTAB_STEREO:
		case RENDERSETTING_STATICTAB_NET:
		case RENDERSETTING_STATICTAB_OVERRIDEMAT:
			return false;
	}
	return true;
}

void CyclesVideoPost::Report(const maxon::BaseArray<maxon::String>& warnings)
{
	for (const maxon::String& warning : warnings)
	{
		if (_reported.Contains(warning))
			continue;
		_reported.Append(warning) iferr_ignore("Repeating a warning is harmless.");
		ApplicationOutput("Cycles Render: @", warning);
	}
}

maxon::Result<void> CyclesVideoPost::OpenSession(const cinema::BaseContainer& settings, cinema::VideoPostStruct* vps)
{
	iferr_scope_handler
	{
		CloseSession(false);
		return err;
	};

	_sync.Reset();
	_client = NewObj(BridgeClient) iferr_return;
	_client->Open(BRIDGEMODE::FINAL) iferr_return;
	if (g_cyclesRenderLogCalls)
		ApplicationOutput("Cycles Render job: Blender started");

	maxon::BaseArray<maxon::String> warnings;
	SendRenderSettings(*_client, settings, vps->doc, vps->thread, warnings) iferr_return;
	Report(warnings);
	return maxon::OK;
}

void CyclesVideoPost::CloseSession(maxon::Bool cleanly)
{
	if (!_client)
		return;
	if (cleanly)
	{
		iferr (_client->Shutdown())
		{
			// Destroying the client below stops Blender in any case.
		}
	}
	DeleteObj(_client);
	_sync.Reset();
	if (g_cyclesRenderLogCalls)
		ApplicationOutput("Cycles Render job: Blender stopped after @ frame(s)", _framesRendered);
	_framesRendered = 0;
	_passesMissingReported = false;
	_reported.Reset();
}

maxon::Result<void> CyclesVideoPost::RenderFrame(const cinema::BaseContainer& settings, cinema::VideoPostStruct* vps, cinema::VPBuffer* rgba)
{
	iferr_scope;

	const maxon::Int32 width = rgba->GetBw();
	const maxon::Int32 height = rgba->GetBh();

	if (!_client)
	{
		OpenSession(settings, vps) iferr_return;
	}

	// Cinema 4D's pixel aspect (Render Settings > Output, or the Output tab of Cycles Render): a
	// pixel is that many times as wide as high. Measured against the Standard renderer: the
	// camera keeps its horizontal view over the width, and the picture is what a picture of
	// width x pixel aspect square pixels shows, squeezed into the width (docs/reference/c4d-sdk.md).
	const maxon::Float64 pixelAspect = GetPixelAspect(vps->render->GetRenderData());

	// The scene: all of it for the job's first frame, then only the items that changed.
	{
		ExportSettings exportSettings = GetExportSettings(settings, width, height);
		exportSettings.pixelAspect = pixelAspect;

		// Motion blur: look at the scene when the shutter opens and when it closes, then come
		// back to the frame. Each look is one evaluation of the document.
		MotionSample shutterOpen;
		MotionSample shutterClose;
		const maxon::Bool motionBlur = exportSettings.motionBlur && exportSettings.shutter > 0.0;
		// The Vector pass without motion blur: Blender takes its motion vectors from where things
		// are one frame before and one frame after (the keys the bridge sets, scene/common.py), so
		// the scene is read at exactly those moments. (With motion blur Cycles has no Vector pass.)
		const maxon::Bool vectorPass = !motionBlur && settings.GetBool(CYCLES_PASS_VECTOR, false);
		if (motionBlur || vectorPass)
		{
			// The picture is the scene in the middle of the open shutter, blurred by what happens
			// from its opening to its closing. With the shutter starting or ending on the frame
			// that middle is half a shutter after or before the frame.
			const maxon::Float64 fps = maxon::Float64(maxon::Max(vps->doc->GetFps(), maxon::Int32(1)));
			const cinema::BaseTime frameTime = cinema::BaseTime(vps->doc->GetTime().Get() + (motionBlur ? exportSettings.shutterCentre / fps : 0.0));
			const maxon::Float64 half = motionBlur ? exportSettings.shutter * 0.5 / fps : 1.0 / fps;
			MotionSample* const samples[2] = { &shutterOpen, &shutterClose };
			for (maxon::Int i = 0; i < 2; ++i)
			{
				vps->doc->SetTime(cinema::BaseTime(frameTime.Get() + (i == 0 ? -half : half)));
				vps->doc->ExecutePasses(vps->thread, true, true, true, cinema::BUILDFLAGS::EXTERNALRENDERER);
				SceneExporter moment(vps->doc, exportSettings);
				moment.Collect() iferr_return;
				moment.GetMotionSample(*samples[i]) iferr_return;
			}
			vps->doc->SetTime(frameTime);
			vps->doc->ExecutePasses(vps->thread, true, true, true, cinema::BUILDFLAGS::EXTERNALRENDERER);
			if (vps->thread && vps->thread->TestBreak())
				return maxon::OperationCancelledError(MAXON_SOURCE_LOCATION);
		}

		else if (vps->renderflags & cinema::RENDERFLAGS::IRR)
		{
			// For the Interactive Render Region Cinema 4D hands over a copy of the document whose
			// generators have not been built: without this a Sphere primitive, for one, is missing.
			vps->doc->ExecutePasses(vps->thread, true, true, true, cinema::BUILDFLAGS::EXTERNALRENDERER);
			if (vps->thread && vps->thread->TestBreak())
				return maxon::OperationCancelledError(MAXON_SOURCE_LOCATION);
		}

		SceneExporter exporter(vps->doc, exportSettings);
		exporter.Collect() iferr_return;
		if (motionBlur || vectorPass)
			exporter.SetMotion(&shutterOpen, &shutterClose);
		if (IsCapturing())
		{
			const maxon::String description = exporter.GetListing() iferr_return;
			Capture("scene", description);
		}
		SceneWriter writer(_sync.GetItemCount() == 0);
		exporter.Write(writer) iferr_return;
		const maxon::Int itemCount = writer.GetItemCount();
		const maxon::Int changes = writer.KeepChanges(_sync) iferr_return;
		writer.Send(*_client, vps->thread) iferr_return;
		Report(writer.GetWarnings());
		if (g_cyclesRenderLogCalls)
			ApplicationOutput("Cycles Render frame: @ of @ scene items sent or removed, rendering @x@", changes, itemCount, width, height);
	}

	JsonWriter request;
	request.BeginObject();
	request.String("op", "render"_s);
	request.Int("width", width);
	request.Int("height", height);
	if (pixelAspect != 1.0)
		request.Float("pixel_aspect", pixelAspect);

	// The passes Cinema 4D has a buffer for.
	maxon::Int passBuffers = 0;
	request.BeginArray("passes");
	for (const PassBuffer& pass : _passes)
	{
		if (!vps->render->GetBuffer(VPBUFFER_POSTEFFECT, pass.bufferId))
			continue;
		request.String(nullptr, maxon::String(pass.pass->name));
		++passBuffers;
	}
	request.EndArray();
	if (!_passes.IsEmpty() && passBuffers == 0 && !_passesMissingReported)
	{
		_passesMissingReported = true;
		ApplicationOutput("Cycles Render: render passes are switched on, but Cinema 4D provides no layers for them. "
			"Enable Multi-Pass in the Render Settings and add \"Post Effects\" to it.");
	}

	// Cinema 4D's render region: borders in pixels from each side of the picture.
	const cinema::BaseContainer renderData = vps->render->GetRenderData();
	if (renderData.GetBool(RDATA_RENDERREGION))
	{
		const maxon::Float64 left = maxon::Float64(renderData.GetInt32(RDATA_RENDERREGION_LEFT)) / width;
		const maxon::Float64 right = 1.0 - maxon::Float64(renderData.GetInt32(RDATA_RENDERREGION_RIGHT)) / width;
		const maxon::Float64 top = 1.0 - maxon::Float64(renderData.GetInt32(RDATA_RENDERREGION_TOP)) / height;
		const maxon::Float64 bottom = maxon::Float64(renderData.GetInt32(RDATA_RENDERREGION_BOTTOM)) / height;
		if (right > left && top > bottom)
		{
			const maxon::Float64 region[4] = { maxon::Clamp01(left), maxon::Clamp01(bottom), maxon::Clamp01(right), maxon::Clamp01(top) };
			request.Floats("region", region, 4);
		}
	}
	const BridgeMessage reply = _client->Request(request, BlobViews(), vps->thread) iferr_return;

	const maxon::Int blobIndex = maxon::Int(GetJsonInt(reply.header, "pixels"_s, -1));
	if (GetJsonInt(reply.header, "width"_s, 0) != width || GetJsonInt(reply.header, "height"_s, 0) != height || GetJsonInt(reply.header, "channels"_s, 0) != 4)
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Blender returned an image of the wrong size."_s);
	if (blobIndex < 0 || blobIndex >= reply.blobs.GetCount())
		return maxon::UnexpectedError(MAXON_SOURCE_LOCATION, "Blender's reply holds no pixels."_s);

	// The reply's rows run top to bottom, like Cinema 4D's buffer.
	// Only the picture itself is converted; the passes stay as rendered. With "Basic" colour
	// management that is what the Standard renderer's own layers hold too: linear values with
	// Linear Workflow (measured: Multi-Pass on makes even its picture linear), the sRGB numbers
	// calculated with without it.
	const OUTPUTCOLORS colors = GetOutputColors(vps->doc, renderData);
	cinema::OcioConverter* converter = nullptr;
	finally
	{
		if (converter)
			DeleteObj(converter);
	};
	if (colors == OUTPUTCOLORS::VIEW)
	{
		iferr (converter = cinema::OcioConverter::Init(vps->doc, nullptr))
			converter = nullptr;
	}
	FillBuffer(rgba, reply.blobs[blobIndex], width, height, colors, converter) iferr_return;

	// The passes follow the picture, one blob each, in the order of the reply's list.
	const maxon::BaseArray<maxon::String> delivered = GetJsonStrings(reply.header, "passes"_s);
	for (maxon::Int i = 0; i < delivered.GetCount(); ++i)
	{
		const maxon::Int passBlob = blobIndex + 1 + i;
		if (passBlob >= reply.blobs.GetCount())
			break;
		for (const PassBuffer& pass : _passes)
		{
			if (maxon::String(pass.pass->name) != delivered[i])
				continue;
			cinema::VPBuffer* const buffer = vps->render->GetBuffer(VPBUFFER_POSTEFFECT, pass.bufferId);
			if (buffer && buffer->GetBw() == width && buffer->GetBh() == height)
			{
				FillBuffer(buffer, reply.blobs[passBlob], width, height) iferr_return;
			}
			break;
		}
	}
	Report(GetJsonStrings(reply.header, "warnings"_s));
	++_framesRendered;
	return maxon::OK;
}

cinema::RENDERRESULT CyclesVideoPost::Execute(cinema::BaseVideoPost* node, cinema::VideoPostStruct* vps)
{
	if (!node || !vps || !vps->doc || !vps->render)
		return cinema::RENDERRESULT::FAILED;

	const maxon::Bool jobCloses = vps->vp == cinema::VIDEOPOSTCALL::FRAMESEQUENCE && !vps->open;

	// An earlier stage already ended the render: keep its verdict (for example "cancelled by
	// user") through the remaining closing calls instead of replacing it.
	if (vps->error && *vps->error != cinema::RENDERRESULT::OK)
	{
		if (jobCloses)
			CloseSession(false);
		return *vps->error;
	}

	if (vps->thread && vps->thread->TestBreak())
	{
		CloseSession(false);
		return cinema::RENDERRESULT::USERBREAK;
	}

	cinema::VPBuffer* const rgba = vps->render->GetBuffer(VPBUFFER_RGBA, NOTOK);

	if (g_cyclesRenderLogCalls)
	{
		ApplicationOutput("Cycles Render call: stage=@ open=@ flags=@ buffer=@x@ bits=@ channels=@",
			cinema::Int32(vps->vp), vps->open, cinema::Int32(vps->renderflags),
			rgba ? rgba->GetBw() : -1, rgba ? rgba->GetBh() : -1, rgba ? rgba->GetBt() : -1, rgba ? rgba->GetCpp() : -1);
	}

	if (jobCloses)
	{
		CloseSession(true);
		return cinema::RENDERRESULT::OK;
	}

	// The frame is rendered when its render stage closes.
	if (vps->vp != cinema::VIDEOPOSTCALL::RENDER || vps->open)
		return cinema::RENDERRESULT::OK;

	if (!rgba)
		return cinema::RENDERRESULT::FAILED;

	iferr (RenderFrame(node->GetDataInstanceRef(), vps, rgba))
	{
		// Whatever went wrong, the connection may be in the middle of a message: start afresh.
		CloseSession(false);
		if (vps->thread && vps->thread->TestBreak())
			return cinema::RENDERRESULT::USERBREAK;
		ApplicationOutput("Cycles Render: the frame could not be rendered. @", err);
		return cinema::RENDERRESULT::FAILED;
	}
	return cinema::RENDERRESULT::OK;
}

cinema::Bool RegisterCyclesVideoPost()
{
	return cinema::RegisterVideoPostPlugin(ID_CYCLES_VIDEOPOST, cinema::GeLoadString(IDS_CYCLESRENDER_NAME),
		PLUGINFLAG_VIDEOPOST_ISRENDERER, CyclesVideoPost::Alloc, "vpcyclesrender"_s, 0, 0);
}

} // namespace cyclesrender
