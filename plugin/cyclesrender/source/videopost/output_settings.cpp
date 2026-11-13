// SPDX-License-Identifier: Apache-2.0
// The Output tab of the Render Settings and the pixel aspect (output_settings.h). Reading order:
// ComputeShown (what the tab shows), then SetOutputParameter (what typing into it changes).

#include "c4d.h"
#include "drendersettings.h"

#include "vpcyclesrender.h"

#include "output_settings.h"

namespace cyclesrender
{

maxon::Float64 GetPixelAspect(const cinema::BaseContainer& renderData)
{
	const maxon::Float64 aspect = renderData.GetFloat(RDATA_PIXELASPECT, 1.0);
	return aspect > 0.0 ? aspect : 1.0;
}

maxon::Bool IsOutputParameter(maxon::Int32 id)
{
	switch (id)
	{
		case CYCLES_OUTPUT_RESOLUTION_X:
		case CYCLES_OUTPUT_RESOLUTION_Y:
		case CYCLES_OUTPUT_PERCENTAGE:
		case CYCLES_OUTPUT_ASPECT_X:
		case CYCLES_OUTPUT_ASPECT_Y:
		case CYCLES_OUTPUT_RESULT:
			return true;
	}
	return false;
}

cinema::RenderData* FindRenderDataOf(const cinema::BaseList2D* videoPost)
{
	const cinema::BaseDocument* const constDoc = videoPost ? videoPost->GetDocument() : nullptr;
	cinema::BaseDocument* const doc = const_cast<cinema::BaseDocument*>(constDoc);
	if (!doc)
		return nullptr;
	// Render settings can be nested: children first, then the next one, then back up.
	for (cinema::RenderData* renderData = doc->GetFirstRenderData(); renderData; )
	{
		for (cinema::BaseVideoPost* post = renderData->GetFirstVideoPost(); post; post = post->GetNext())
		{
			if (post == videoPost)
				return renderData;
		}
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
	return nullptr;
}

// Blender's limits: a percentage of 1 to 1000 here (Blender allows more), pixel aspects of 1 to 200.
static const maxon::Int32 MAX_PERCENTAGE = 1000;
static const maxon::Int32 MAX_RESOLUTION = 65536;

// A size scaled by the percentage, as Blender rounds it.
static maxon::Int32 Scaled(maxon::Int32 size, maxon::Int32 percentage)
{
	return maxon::ClampValue(maxon::Int32(maxon::Float64(size) * maxon::Float64(percentage) / 100.0 + 0.5), maxon::Int32(1), MAX_RESOLUTION);
}

struct OutputValues
{
	maxon::Int32 resolutionX = 1920;
	maxon::Int32 resolutionY = 1080;
	maxon::Int32 percentage = 100;
	maxon::Float64 aspectX = 1.0;
	maxon::Float64 aspectY = 1.0;
};

// What the tab shows: its own values where they give Cinema 4D's, otherwise Cinema 4D's own
// in Blender's terms (resolution unscaled by the percentage, the aspect as a ratio of whole
// pixels where it is one).
static OutputValues ComputeShown(const cinema::BaseContainer& own, const cinema::RenderData* renderData)
{
	OutputValues shown;
	shown.resolutionX = maxon::ClampValue(own.GetInt32(CYCLES_OUTPUT_RESOLUTION_X, 1920), maxon::Int32(1), MAX_RESOLUTION);
	shown.resolutionY = maxon::ClampValue(own.GetInt32(CYCLES_OUTPUT_RESOLUTION_Y, 1080), maxon::Int32(1), MAX_RESOLUTION);
	shown.percentage = maxon::ClampValue(own.GetInt32(CYCLES_OUTPUT_PERCENTAGE, 100), maxon::Int32(1), MAX_PERCENTAGE);
	shown.aspectX = maxon::ClampValue(own.GetFloat(CYCLES_OUTPUT_ASPECT_X, 1.0), 1.0, 200.0);
	shown.aspectY = maxon::ClampValue(own.GetFloat(CYCLES_OUTPUT_ASPECT_Y, 1.0), 1.0, 200.0);
	if (!renderData)
		return shown;

	const cinema::BaseContainer& data = renderData->GetDataInstanceRef();
	const maxon::Int32 width = maxon::Max(maxon::Int32(1), data.GetInt32(RDATA_XRES, 1280));
	const maxon::Int32 height = maxon::Max(maxon::Int32(1), data.GetInt32(RDATA_YRES, 720));
	const maxon::Float64 aspect = GetPixelAspect(data);
	if (Scaled(shown.resolutionX, shown.percentage) != width)
		shown.resolutionX = maxon::ClampValue(maxon::Int32(maxon::Float64(width) * 100.0 / shown.percentage + 0.5), maxon::Int32(1), MAX_RESOLUTION);
	if (Scaled(shown.resolutionY, shown.percentage) != height)
		shown.resolutionY = maxon::ClampValue(maxon::Int32(maxon::Float64(height) * 100.0 / shown.percentage + 0.5), maxon::Int32(1), MAX_RESOLUTION);
	if (maxon::Abs(shown.aspectX / shown.aspectY - aspect) > 1e-6 * aspect)
	{
		// Blender's aspect values start at 1: the wider side gets the ratio.
		shown.aspectX = aspect >= 1.0 ? maxon::Min(aspect, 200.0) : 1.0;
		shown.aspectY = aspect >= 1.0 ? 1.0 : maxon::Min(1.0 / aspect, 200.0);
	}
	return shown;
}

maxon::Bool GetOutputParameter(const cinema::BaseList2D* videoPost, maxon::Int32 id, cinema::GeData& value)
{
	if (!videoPost || !IsOutputParameter(id))
		return false;
	const cinema::RenderData* const renderData = FindRenderDataOf(videoPost);
	const OutputValues shown = ComputeShown(videoPost->GetDataInstanceRef(), renderData);
	switch (id)
	{
		case CYCLES_OUTPUT_RESOLUTION_X: value = cinema::GeData(shown.resolutionX); break;
		case CYCLES_OUTPUT_RESOLUTION_Y: value = cinema::GeData(shown.resolutionY); break;
		case CYCLES_OUTPUT_PERCENTAGE: value = cinema::GeData(shown.percentage); break;
		case CYCLES_OUTPUT_ASPECT_X: value = cinema::GeData(shown.aspectX); break;
		case CYCLES_OUTPUT_ASPECT_Y: value = cinema::GeData(shown.aspectY); break;
		default:
		{
			// The line next to the percentage: what renders.
			const maxon::Int32 width = Scaled(shown.resolutionX, shown.percentage);
			const maxon::Int32 height = Scaled(shown.resolutionY, shown.percentage);
			value = cinema::GeData(FormatString("= @ x @ px", width, height));
			break;
		}
	}
	return true;
}

// Sets a size of Cinema 4D's render data through its description, as its own page does, only
// when the value changes.
static void SetRenderParameter(cinema::RenderData* renderData, maxon::Int32 id, maxon::Int32 value, maxon::Int32 current)
{
	if (value == current)
		return;
	renderData->SetParameter(CreateDescID(cinema::DescLevel(id)), cinema::GeData(value), cinema::DESCFLAGS_SET::NONE);
}

maxon::Bool SetOutputParameter(cinema::BaseList2D* videoPost, maxon::Int32 id, const cinema::GeData& value)
{
	if (!videoPost || !IsOutputParameter(id))
		return false;
	if (id == CYCLES_OUTPUT_RESULT)
		return true;	// a line of text; nothing to store

	cinema::RenderData* const renderData = FindRenderDataOf(videoPost);
	cinema::BaseContainer& own = videoPost->GetDataInstanceRef();
	// Start from what the tab shows, so that a value changed on Cinema 4D's page is kept.
	OutputValues values = ComputeShown(own, renderData);
	switch (id)
	{
		case CYCLES_OUTPUT_RESOLUTION_X: values.resolutionX = maxon::ClampValue(value.GetInt32(), maxon::Int32(1), MAX_RESOLUTION); break;
		case CYCLES_OUTPUT_RESOLUTION_Y: values.resolutionY = maxon::ClampValue(value.GetInt32(), maxon::Int32(1), MAX_RESOLUTION); break;
		case CYCLES_OUTPUT_PERCENTAGE: values.percentage = maxon::ClampValue(value.GetInt32(), maxon::Int32(1), MAX_PERCENTAGE); break;
		case CYCLES_OUTPUT_ASPECT_X: values.aspectX = maxon::ClampValue(value.GetFloat(), 1.0, 200.0); break;
		default: values.aspectY = maxon::ClampValue(value.GetFloat(), 1.0, 200.0); break;
	}
	own.SetInt32(CYCLES_OUTPUT_RESOLUTION_X, values.resolutionX);
	own.SetInt32(CYCLES_OUTPUT_RESOLUTION_Y, values.resolutionY);
	own.SetInt32(CYCLES_OUTPUT_PERCENTAGE, values.percentage);
	own.SetFloat(CYCLES_OUTPUT_ASPECT_X, values.aspectX);
	own.SetFloat(CYCLES_OUTPUT_ASPECT_Y, values.aspectY);
	if (!renderData)
		return true;

	const cinema::BaseContainer& data = renderData->GetDataInstanceRef();
	SetRenderParameter(renderData, RDATA_XRES, Scaled(values.resolutionX, values.percentage), data.GetInt32(RDATA_XRES));
	SetRenderParameter(renderData, RDATA_YRES, Scaled(values.resolutionY, values.percentage), data.GetInt32(RDATA_YRES));
	// Setting the pixel aspect this way also chooses Cinema 4D's preset for it (measured: 2 chose
	// "Anamorphic"), as typing it into its own page does.
	const maxon::Float64 aspect = values.aspectX / values.aspectY;
	if (maxon::Abs(aspect - GetPixelAspect(data)) > 1e-9)
		renderData->SetParameter(CreateDescID(cinema::DescLevel(RDATA_PIXELASPECT)), cinema::GeData(aspect), cinema::DESCFLAGS_SET::NONE);
	renderData->SetDirty(cinema::DIRTYFLAGS::DATA);
	return true;
}

} // namespace cyclesrender
