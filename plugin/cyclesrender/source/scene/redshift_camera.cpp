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
#include "redshift_camera.h"
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

	// What else the camera does to its picture before the view (W7 of internal/f13/parity-report.md): not translated.
	if (GetBoolParameter(camera, RSCAMERAOBJECT_TONE_MAPPING_ENABLED, false))
	{
		exposure.notes.Append(FormatString("Camera \"@\": Redshift's tone mapping is not translated.", name)) iferr_return;
	}
	if (GetFloatParameter(camera, RSCAMERAOBJECT_VIGNETTING, 0.0) > 0.0)
	{
		exposure.notes.Append(FormatString("Camera \"@\": Redshift's vignetting is not translated.", name)) iferr_return;
	}
	const cinema::Vector white = GetVectorParameter(camera, RSCAMERAOBJECT_WHITE_POINT, cinema::Vector(1.0));
	if (maxon::Abs(white.x - 1.0) > 1e-3 || maxon::Abs(white.y - 1.0) > 1e-3 || maxon::Abs(white.z - 1.0) > 1e-3)
	{
		exposure.notes.Append(FormatString("Camera \"@\": Redshift's white point is not translated.", name)) iferr_return;
	}
	return exposure;
}

} // namespace cyclesrender
