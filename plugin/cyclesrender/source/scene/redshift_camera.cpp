// SPDX-License-Identifier: Apache-2.0
// The exposure of a Redshift camera (redshift_camera.h). The measurements behind every number here are in
// docs/reference/c4d-sdk.md, "The Redshift camera's exposure" (internal/f13/probe_w6.py).

#include "c4d.h"
#include "drendersettings.h"
#include "maxon/atomictypes.h"
#include "orscamera.h"

#include "../sdk_compat.h"
#include "light_export.h"
#include "parameters.h"
#include "imager_export.h"
#include "redshift_camera.h"
#include "tcyclescamera.h"
#include "redshift_ids.h"

namespace cyclesrender
{

// The parameters read, as the four SDKs' orscamera.h number them (enum members counted from 7000, 8100 and
// 8300 there): the numbers were measured with these values, so a header that moved one would be noticed here.
static_assert(RSCAMERAOBJECT_EXPOSURE_TYPE == 7016 && RSCAMERAOBJECT_SHUTTER_TYPE == 7017 && RSCAMERAOBJECT_WHITE_POINT == 7019,
	"the Redshift camera's exposure ids moved");
static_assert(RSCAMERAOBJECT_SHUTTER_TIME_RATIO == 8101 && RSCAMERAOBJECT_TONE_MAPPING_ENABLED == 8301, "the Redshift camera's shutter ids moved");

// "Filmic" at EV 0, ISO 100, a 60th of a second and f/8 (the values of a new camera): measured 2.8711 to 2.8716 at
// dome values 0.02 to 1, the same in Redshift 3.6.04, 2026.3 and 2027.0 under the Units Mode "Disabled" or "Custom".
static const maxon::Float64 FILMIC_FACTOR = 2.8715;
// Under the Units Mode "Project Settings" (Redshift 2026 on), "Filmic" is 683/10000 as bright (0.1961 measured),
// whatever the document's unit, the Export Units or "Image Units Scale": the pair of constants Redshift's
// photometric units also have between the two modes (light_export.cpp, RedshiftUnitsModeFactor).
static const maxon::Float64 PROJECT_SETTINGS_FACTOR = 683.0 / 10000.0;
// The least the factor is written as (a factor of 0 has no stops): Blender's picture is black there.
static const maxon::Float64 DARKEST_STOPS = -100.0;

// Whether this Redshift has the Units Mode in its render settings (2026 and 2027 do; Redshift 3.6.04, Cinema 4D
// 2024's, has "Legacy Units" instead, and its "Filmic" counts with FILMIC_FACTOR whatever they say, measured). Asked
// once from the description of Redshift's renderer: Redshift does not change in a session.
static maxon::Bool HasUnitsMode(const cinema::BaseVideoPost* post)
{
	static maxon::AtomicInt32 known;   // 0 not yet asked, 1 yes, 2 no
	const maxon::Int32 state = known.Load();
	if (state != 0)
		return state == 1;
	maxon::Bool found = false;
	cinema::BaseVideoPost* made = post ? nullptr : cinema::BaseVideoPost::Alloc(REDSHIFT_RENDERER);
	const cinema::BaseVideoPost* const asked = post ? post : made;
	cinema::AutoAlloc<cinema::Description> description;
	if (asked && description && asked->GetDescription(description, cinema::DESCFLAGS_DESC::NONE))
		found = description->GetParameterI(ConstDescIDLevel(REDSHIFT_RENDERER_UNIT_MODE), nullptr) != nullptr;
	if (made)
		cinema::BaseVideoPost::Free(made);
	// Without Redshift there is nothing to ask, and nothing to remember.
	if (asked)
		known.Set(found ? 1 : 2);
	return found;
}

// "Filmic"'s constant for this document: Redshift's Units Mode decides, where it has one. Without Redshift's render
// settings in the document it is what new ones have ("Project Settings").
static maxon::Float64 FilmicFactor(const cinema::BaseDocument* doc)
{
	const cinema::BaseVideoPost* const post = FindRedshiftRenderer(doc);
	if (!HasUnitsMode(post))
		return FILMIC_FACTOR;
	const maxon::Int32 mode = post ? GetIntParameter(post, REDSHIFT_RENDERER_UNIT_MODE, REDSHIFT_RENDERER_UNIT_MODE_PROJECT)
		: REDSHIFT_RENDERER_UNIT_MODE_PROJECT;
	return mode == REDSHIFT_RENDERER_UNIT_MODE_PROJECT ? FILMIC_FACTOR * PROJECT_SETTINGS_FACTOR : FILMIC_FACTOR;
}

// The exposure time "Filmic" counts with, in seconds; 0 when the camera's values give none.
static maxon::Float64 ExposureTime(const cinema::BaseObject* camera)
{
	if (GetIntParameter(camera, RSCAMERAOBJECT_SHUTTER_TYPE, RSCAMERAOBJECT_SHUTTER_TYPE_STILL) == RSCAMERAOBJECT_SHUTTER_TYPE_MOVIE)
	{
		// "Movie": the shutter is open for its angle's share of a frame, at the frame rate of the render settings (not the
		// document's: in Cinema 4D 2024, where the two can differ, Redshift followed the render settings' one; measured).
		const cinema::BaseDocument* const doc = camera->GetDocument();
		const cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
		maxon::Float64 rate = renderData ? GetFloatParameter(renderData, RDATA_FRAMERATE, 0.0) : 0.0;
		if (rate <= 0.0 && doc)
			rate = maxon::Float64(doc->GetFps());
		const maxon::Float64 angle = GetFloatParameter(camera, RSCAMERAOBJECT_SHUTTER_ANGLE, maxon::PI);
		return rate > 0.0 && angle > 0.0 ? angle / (maxon::PI2 * rate) : 0.0;
	}
	// "Still": the Shutter Time is the denominator of the time, 60 for a 60th of a second.
	const maxon::Float64 denominator = GetFloatParameter(camera, RSCAMERAOBJECT_SHUTTER_TIME_RATIO, 60.0);
	return denominator > 0.0 ? 1.0 / denominator : 0.0;
}

maxon::Result<RedshiftExposure> ReadRedshiftExposure(const cinema::BaseObject* camera)
{
	iferr_scope;
	RedshiftExposure exposure;
	if (!camera || camera->GetType() != Orscamera)
		return exposure;
	const maxon::String name = camera->GetName();
	maxon::Float64 factor = maxon::Pow(2.0, GetFloatParameter(camera, RSCAMERAOBJECT_EXPOSURE, 0.0));
	const maxon::Int32 type = GetIntParameter(camera, RSCAMERAOBJECT_EXPOSURE_TYPE, RSCAMERAOBJECT_EXPOSURE_TYPE_EV_ONLY);
	if (type == RSCAMERAOBJECT_EXPOSURE_TYPE_FILMIC)
	{
		const maxon::Float64 iso = GetFloatParameter(camera, RSCAMERAOBJECT_ISO, 100.0);
		const maxon::Float64 fNumber = GetFloatParameter(camera, RSCAMERAOBJECT_FNUMBER_VALUE, 8.0);
		const maxon::Float64 seconds = ExposureTime(camera);
		if (iso > 0.0 && fNumber > 0.0 && seconds > 0.0)
		{
			factor *= FilmicFactor(camera->GetDocument()) * (iso / 100.0) * (60.0 * seconds) * maxon::Sqr(8.0 / fNumber);
		}
		else
		{
			exposure.notes.Append(FormatString("Camera \"@\": its \"Filmic\" exposure has no exposure time, sensitivity or aperture; only its EV is translated.", name)) iferr_return;
		}
	}
	else if (type != RSCAMERAOBJECT_EXPOSURE_TYPE_EV_ONLY)
	{
		// The SDKs from 2026 on list an automatic type, which Redshift 2027.0 does not offer [untested].
		exposure.notes.Append(FormatString("Camera \"@\": its exposure type @ is not translated; only its EV is.", name, type)) iferr_return;
	}
	exposure.stops = factor > 0.0 ? maxon::Max(maxon::Log2(factor), DARKEST_STOPS) : DARKEST_STOPS;
	return exposure;
}

//----------------------------------------------------------------------------------------
// The effects after the exposure (ReadRedshiftEffects). Measured with Redshift 2027.0.0 in an OCIO document, where
// Redshift returns render-space values before the view (internal/f13/probe_w7.py; docs/reference/c4d-sdk.md, "The
// Redshift camera's effects", has every number).
//----------------------------------------------------------------------------------------

// RS Post-Effects, the video post, and its groups (drsposteffects.h of the four SDKs; the Redshift camera's own groups
// have the same numbers, orscamera.h: "IDs match those from drsposteffects.h wherever possible").
static const maxon::Int32 RS_POST_EFFECTS = 1040189;
enum
{
	RSPOST_BLOOM_ENABLED = 12100, RSPOST_BLOOM_THRESHOLD = 12101, RSPOST_BLOOM_INTENSITY = 12103, RSPOST_BLOOM_TINT_FIRST = 12104,
	RSPOST_FLARE_ENABLED = 12200, RSPOST_FLARE_THRESHOLD = 12201, RSPOST_FLARE_INTENSITY = 12206, RSPOST_FLARE_TINT_FIRST = 12207,
	RSPOST_STREAK_ENABLED = 12300, RSPOST_STREAK_THRESHOLD = 12301, RSPOST_STREAK_TAIL = 12302, RSPOST_STREAK_NUMBER = 12304,
	RSPOST_STREAK_ANGLE = 12305, RSPOST_STREAK_INTENSITY = 12306,
	RSPOST_LUT_ENABLED = 12400,
	RSPOST_COLORCONTROL_ENABLED = 12500, RSPOST_COLORCONTROL_EXPOSURE = 12501, RSPOST_COLORCONTROL_CONTRAST = 12502,
	// The Redshift camera's switch per group: 0 "Render Settings" (the video post's), 1 "Off", 2 "Override" (the camera's own).
	RSCAMERA_BLOOM_MODE = 12110, RSCAMERA_FLARE_MODE = 12214, RSCAMERA_STREAK_MODE = 12307, RSCAMERA_LUT_MODE = 12407, RSCAMERA_COLORCONTROL_MODE = 12510,
	RSCAMERA_MODE_RENDER_SETTINGS = 0, RSCAMERA_MODE_OFF = 1, RSCAMERA_MODE_OVERRIDE = 2,
	RSCAMERA_SATURATION = 7020, RSCAMERA_TONE_BLACKS = 8304
};

// The RS Post-Effects video post of the active render data, when it is there and switched on.
static const cinema::BaseVideoPost* FindPostEffects(const cinema::BaseDocument* doc)
{
	const cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
	for (const cinema::BaseVideoPost* post = renderData ? FirstVideoPost(renderData) : nullptr; post; post = post->GetNext())
	{
		if (post->GetType() == RS_POST_EFFECTS)
			return post->GetBit(BIT_VPDISABLED) ? nullptr : post;
	}
	return nullptr;
}

// Where a group of post effects is read for @p camera: the camera's own (its group set to "Override"), the video post
// (a Cinema 4D camera, or a Redshift camera's group at "Render Settings") when its group is switched on there, or nowhere.
static const cinema::BaseList2D* GroupSource(const cinema::BaseObject* camera, const cinema::BaseVideoPost* post, maxon::Int32 mode, maxon::Int32 enabled)
{
	if (camera->GetType() == Orscamera)
	{
		const maxon::Int32 chosen = GetIntParameter(camera, mode, RSCAMERA_MODE_RENDER_SETTINGS);
		if (chosen == RSCAMERA_MODE_OFF)
			return nullptr;
		if (chosen == RSCAMERA_MODE_OVERRIDE)
			return camera;
	}
	return post && GetBoolParameter(post, enabled, false) ? post : nullptr;
}

// True when one of the @p count tint colours from @p first on is not white.
static maxon::Bool Tinted(const cinema::BaseList2D* source, maxon::Int32 first, maxon::Int32 count)
{
	for (maxon::Int32 i = 0; i < count; ++i)
	{
		const cinema::Vector tint = GetVectorParameter(source, first + i, cinema::Vector(1.0));
		if (maxon::Abs(tint.x - 1.0) > 1e-3 || maxon::Abs(tint.y - 1.0) > 1e-3 || maxon::Abs(tint.z - 1.0) > 1e-3)
			return true;
	}
	return false;
}

// Redshift's bloom, streak and flare as Blender's Glare node (look-alikes; docs/reference/blender.md, "Lens Glare", and
// docs/reference/c4d-sdk.md, "The Redshift camera's effects"). Redshift's threshold T is a soft knee: of a pixel x it
// blooms about 0.03 to 0.05 at 0.1 T, 0.35 at 0.4 T, 0.8 at T and 0.92 to 1 from 2 T on (a uniform field of 2 T is
// doubled). Blender's Glare with Smoothness 1 blooms x / (4 T') below 2 T' and 1 - T' / x above (measured on uniform
// pictures): with T' a quarter of T that is 0.1, 0.4, 0.75 and 0.875 at those points. Its bloom then adds 1.0 of a
// uniform field's light and 0.72 to 0.82 of a bright patch's where Redshift adds 1.0 and 0.92 to 0.96: the intensity
// is 1.2 times Redshift's. Redshift's streak adds 0.58 (3 streaks) and 0.78 (4) of the patch, Blender's Streaks
// (5 iterations) 0.74 and 0.98: 0.79 times.
static const maxon::Float64 GLARE_THRESHOLD_SHARE = 0.25;
static const maxon::Float64 GLARE_SMOOTHNESS = 1.0;
static const maxon::Float64 BLOOM_GAIN = 1.2;
static const maxon::Float64 STREAK_GAIN = 0.79;

maxon::Result<void> ReadRedshiftEffects(const cinema::BaseObject* camera, ImagerChain& chain, const RedshiftTranslation& which)
{
	iferr_scope;
	if (!camera || !which.all)
		return maxon::OK;
	const cinema::BaseDocument* const doc = camera->GetDocument();
	const cinema::BaseVideoPost* const post = FindPostEffects(doc);
	const maxon::Bool redshiftCamera = camera->GetType() == Orscamera;
	if (!redshiftCamera && !post)
		return maxon::OK;
	const maxon::String name = camera->GetName();

	if (redshiftCamera)
	{
		// The White Point (measured: before the tone mapping; grey keeps its brightness).
		const cinema::Vector white = GetVectorParameter(camera, RSCAMERAOBJECT_WHITE_POINT, cinema::Vector(1.0));
		if (which.whitePoint && (maxon::Abs(white.x - white.y) > 1e-4 || maxon::Abs(white.y - white.z) > 1e-4))
		{
			chain.AppendLayer("White Balance", IMAGERLAYER_SOURCE_BUILT, "Redshift White Point"_s, { { "White Point", { white.x, white.y, white.z }, nullptr } }) iferr_return;
		}
		// The vignetting (before the tone mapping).
		const maxon::Float64 vignetting = GetFloatParameter(camera, RSCAMERAOBJECT_VIGNETTING, 0.0);
		if (which.vignetting && vignetting > 0.0)
		{
			// Measured exact: cos(theta)^v, theta the angle of the pixel's ray to the camera's axis, the sensor's width spread
			// over the picture's width (whatever its aspect): "Lens Vignetting" takes the angle of view across the longer side.
			const cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
			// (From the container: the parameter is a number with a fraction in 2027 and a whole one before; the container gives either.)
			const maxon::Float64 width = renderData ? renderData->GetDataInstanceRef().GetFloat(RDATA_XRES) : 0.0;
			const maxon::Float64 height = renderData ? renderData->GetDataInstanceRef().GetFloat(RDATA_YRES) : 0.0;
			const maxon::Float64 longer = width > 0.0 && height > width ? height / width : 1.0;
			const maxon::Float64 focal = GetFloatParameter(camera, RSCAMERAOBJECT_FOCAL_LENGTH, 36.0);
			const maxon::Float64 sensor = GetFloatParameter(camera, RSCAMERAOBJECT_APERTURE, 36.0);
			const maxon::Float64 view = focal > 0.0 ? 2.0 * maxon::ATan(0.5 * sensor * longer / focal) : 0.9273;
			chain.AppendLayer("Lens Vignetting", IMAGERLAYER_SOURCE_BUILT, "Redshift Vignetting"_s, { { "Strength", { vignetting, 0, 0 }, nullptr },
				{ "Field of View", { view, 0, 0 }, nullptr } }) iferr_return;
		}
	}

	// Bloom, streak and flare (measured: after the exposure, before the tone mapping and the view).
	if (const cinema::BaseList2D* const bloom = which.bloom ? GroupSource(camera, post, RSCAMERA_BLOOM_MODE, RSPOST_BLOOM_ENABLED) : nullptr)
	{
		chain.AppendLayer("Lens Glare", IMAGERLAYER_SOURCE_BUILT, "Redshift Bloom"_s, { { "Type", { 0, 0, 0 }, "Bloom" },
			{ "Threshold", { GetFloatParameter(bloom, RSPOST_BLOOM_THRESHOLD, 20.0) * GLARE_THRESHOLD_SHARE, 0, 0 }, nullptr }, { "Smoothness", { GLARE_SMOOTHNESS, 0, 0 }, nullptr },
			{ "Size", { 1.0, 0, 0 }, nullptr }, { "Intensity", { GetFloatParameter(bloom, RSPOST_BLOOM_INTENSITY, 1.0) * BLOOM_GAIN, 0, 0 }, nullptr } }) iferr_return;
		if (Tinted(bloom, RSPOST_BLOOM_TINT_FIRST, 5))
		{
			chain.notes.Append(FormatString("Camera \"@\": the tint of Redshift's bloom is not translated.", name)) iferr_return;
		}
	}
	if (const cinema::BaseList2D* const streak = which.streak ? GroupSource(camera, post, RSCAMERA_STREAK_MODE, RSPOST_STREAK_ENABLED) : nullptr)
	{
		chain.AppendLayer("Lens Glare", IMAGERLAYER_SOURCE_BUILT, "Redshift Streak"_s, { { "Type", { 0, 0, 0 }, "Streaks" },
			{ "Threshold", { GetFloatParameter(streak, RSPOST_STREAK_THRESHOLD, 20.0) * GLARE_THRESHOLD_SHARE, 0, 0 }, nullptr }, { "Smoothness", { GLARE_SMOOTHNESS, 0, 0 }, nullptr },
			{ "Streaks", { maxon::Float64(GetIntParameter(streak, RSPOST_STREAK_NUMBER, 3)), 0, 0 }, nullptr },
			{ "Streaks Angle", { GetFloatParameter(streak, RSPOST_STREAK_ANGLE, 0.349), 0, 0 }, nullptr },
			{ "Iterations", { 5.0, 0, 0 }, nullptr },
			{ "Intensity", { GetFloatParameter(streak, RSPOST_STREAK_INTENSITY, 1.0) * STREAK_GAIN, 0, 0 }, nullptr } }) iferr_return;
		if (maxon::Abs(GetFloatParameter(streak, RSPOST_STREAK_TAIL, 0.5) - 0.5) > 1e-3)
		{
			chain.notes.Append(FormatString("Camera \"@\": the tail of Redshift's streak is not translated.", name)) iferr_return;
		}
	}
	if (const cinema::BaseList2D* const flare = which.flare ? GroupSource(camera, post, RSCAMERA_FLARE_MODE, RSPOST_FLARE_ENABLED) : nullptr)
	{
		// Not measured against Redshift: its flare gives a picture of NaN in every pixel to RenderDocument (2027.0.0).
		chain.AppendLayer("Lens Glare", IMAGERLAYER_SOURCE_BUILT, "Redshift Flare"_s, { { "Type", { 0, 0, 0 }, "Ghosts" },
			{ "Threshold", { GetFloatParameter(flare, RSPOST_FLARE_THRESHOLD, 20.0) * GLARE_THRESHOLD_SHARE, 0, 0 }, nullptr }, { "Smoothness", { GLARE_SMOOTHNESS, 0, 0 }, nullptr },
			{ "Intensity", { GetFloatParameter(flare, RSPOST_FLARE_INTENSITY, 0.2), 0, 0 }, nullptr } }) iferr_return;
		if (Tinted(flare, RSPOST_FLARE_TINT_FIRST, 6))
		{
			chain.notes.Append(FormatString("Camera \"@\": the tint of Redshift's flare is not translated.", name)) iferr_return;
		}
	}

	// The tone mapping and the saturation it carries (a Redshift camera's; the saturation does nothing without it, measured).
	if (which.toneMapping && redshiftCamera && GetBoolParameter(camera, RSCAMERAOBJECT_TONE_MAPPING_ENABLED, false))
	{
		chain.AppendLayer("Tone Mapping", IMAGERLAYER_SOURCE_BUILT, "Redshift Tone Mapping"_s, {
			{ "Highlights", { GetFloatParameter(camera, RSCAMERAOBJECT_TONE_MAPPING_HIGHLIGHTS, 0.2), 0, 0 }, nullptr },
			{ "Desaturate Highlights", { GetBoolParameter(camera, RSCAMERAOBJECT_TONE_MAPPING_DESATURATE_HIGHLIGHTS, true) ? 1.0 : 0.0, 0, 0 }, nullptr },
			{ "Saturation", { GetFloatParameter(camera, RSCAMERA_SATURATION, 0.0), 0, 0 }, nullptr } }) iferr_return;
		if (GetFloatParameter(camera, RSCAMERA_TONE_BLACKS, 0.0) > 0.0)
		{
			chain.notes.Append(FormatString("Camera \"@\": the Blacks of Redshift's tone mapping are not translated.", name)) iferr_return;
		}
	}

	// The colour controls act after the view (measured: they clip at the view's brightest), so only their exposure is
	// translated, before the view (within 5 % of Redshift from 0.01 to 2, exact at 0.18); a Redshift camera has none of it.
	if (const cinema::BaseList2D* const controls = which.colorControls ? GroupSource(camera, post, RSCAMERA_COLORCONTROL_MODE, RSPOST_COLORCONTROL_ENABLED) : nullptr)
	{
		const maxon::Float64 stops = controls == post && !redshiftCamera ? GetFloatParameter(post, RSPOST_COLORCONTROL_EXPOSURE, 0.0) : 0.0;
		if (stops != 0.0)
		{
			chain.AppendLayer("CompositorNodeExposure", IMAGERLAYER_SOURCE_NODE, "Redshift Color Controls Exposure"_s, { { "Exposure", { stops, 0, 0 }, nullptr } }) iferr_return;
		}
		if (GetFloatParameter(controls, RSPOST_COLORCONTROL_CONTRAST, 0.0) != 0.0)
		{
			chain.notes.Append(FormatString("Camera \"@\": the contrast of Redshift's color controls is not translated.", name)) iferr_return;
		}
		chain.notes.Append(FormatString("Camera \"@\": the curves of Redshift's color controls are not translated (where they were changed).", name)) iferr_return;
	}

	// The LUT (measured: after the view; Blender's compositor has no LUT node in 5.0 to 5.3; left for later, the maintainer
	// 2026-11-25). It has no switch of its own: named whenever the translation is on.
	if (GroupSource(camera, post, RSCAMERA_LUT_MODE, RSPOST_LUT_ENABLED))
	{
		chain.notes.Append(FormatString("Camera \"@\": Redshift's LUT is not translated.", name)) iferr_return;
	}
	return maxon::OK;
}

} // namespace cyclesrender
