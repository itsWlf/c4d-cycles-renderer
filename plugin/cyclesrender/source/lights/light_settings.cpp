// SPDX-License-Identifier: Apache-2.0
// Blender's light settings shared by the Cycles Light object and tag (light_settings.h). Reading
// order: the defaults, what the Attribute Manager shows, then reading and storing the values.

#include "c4d.h"

#include "ocycleslight.h"
#include "tcycleslight.h"

#include "../scene/parameters.h"
#include "light_settings.h"

namespace cyclesrender
{

// One reader serves the object and the tag only while both descriptions give a setting the same
// number.
static_assert(maxon::Int32(CYCLESLIGHT_TYPE) == maxon::Int32(CYCLESLIGHTTAG_TYPE) && maxon::Int32(CYCLESLIGHT_SPREAD) == maxon::Int32(CYCLESLIGHTTAG_SPREAD) &&
	maxon::Int32(CYCLESLIGHT_SPOT_BLEND) == maxon::Int32(CYCLESLIGHTTAG_SPOT_BLEND) && maxon::Int32(CYCLESLIGHT_SIZE_Y) == maxon::Int32(CYCLESLIGHTTAG_SIZE_Y) &&
	maxon::Int32(CYCLESLIGHT_PORTAL) == maxon::Int32(CYCLESLIGHTTAG_PORTAL) &&
	maxon::Int32(CYCLESLIGHT_VISIBLE_VOLUME_SCATTER) == maxon::Int32(CYCLESLIGHTTAG_VISIBLE_VOLUME_SCATTER) &&
	maxon::Int32(CYCLESLIGHT_EEVEE_SHADOW_ABSOLUTE) == maxon::Int32(CYCLESLIGHTTAG_EEVEE_SHADOW_ABSOLUTE) &&
	maxon::Int32(CYCLESLIGHT_GROUP_BEAM) == maxon::Int32(CYCLESLIGHTTAG_GROUP_BEAM) &&
	maxon::Int32(CYCLESLIGHT_GROUP_EEVEE_DISTANCE) == maxon::Int32(CYCLESLIGHTTAG_GROUP_EEVEE_DISTANCE) &&
	maxon::Int32(CYCLESLIGHT_SHAPE_ELLIPSE) == maxon::Int32(CYCLESLIGHTTAG_SHAPE_ELLIPSE),
	"the Cycles Light object and tag must use the same parameter ids");

void InitLightSettings(cinema::BaseContainer& data)
{
	data.SetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT);
	data.SetBool(CYCLESLIGHT_USE_TEMPERATURE, false);
	data.SetFloat(CYCLESLIGHT_TEMPERATURE, LIGHTSETTINGS_DEFAULT::TEMPERATURE);
	data.SetVector(CYCLESLIGHT_COLOR, cinema::Vector(1.0));
	data.SetFloat(CYCLESLIGHT_ENERGY, LIGHTSETTINGS_DEFAULT::ENERGY);
	data.SetFloat(CYCLESLIGHT_EXPOSURE, 0.0);
	data.SetBool(CYCLESLIGHT_NORMALIZE, true);
	data.SetFloat(CYCLESLIGHT_RADIUS, 0.0);
	data.SetBool(CYCLESLIGHT_SOFT_FALLOFF, true);
	data.SetFloat(CYCLESLIGHT_ANGLE, LIGHTSETTINGS_DEFAULT::ANGLE);
	data.SetInt32(CYCLESLIGHT_SHAPE, CYCLESLIGHT_SHAPE_SQUARE);
	data.SetFloat(CYCLESLIGHT_SIZE, LIGHTSETTINGS_DEFAULT::SIZE);
	data.SetFloat(CYCLESLIGHT_SIZE_Y, LIGHTSETTINGS_DEFAULT::SIZE);
	data.SetFloat(CYCLESLIGHT_SPOT_SIZE, LIGHTSETTINGS_DEFAULT::SPOT_SIZE);
	data.SetFloat(CYCLESLIGHT_SPOT_BLEND, LIGHTSETTINGS_DEFAULT::SPOT_BLEND);
	data.SetBool(CYCLESLIGHT_SHOW_CONE, false);
	data.SetFloat(CYCLESLIGHT_SPREAD, LIGHTSETTINGS_DEFAULT::SPREAD);

	data.SetInt32(CYCLESLIGHT_MAX_BOUNCES, LIGHTSETTINGS_DEFAULT::MAX_BOUNCES);
	data.SetBool(CYCLESLIGHT_CAST_SHADOW, true);
	data.SetBool(CYCLESLIGHT_MULTIPLE_IMPORTANCE, true);
	data.SetBool(CYCLESLIGHT_SHADOW_CAUSTICS, false);
	data.SetBool(CYCLESLIGHT_PORTAL, false);

	// A light Blender adds is not seen by the camera (visible_camera false, measured).
	data.SetBool(CYCLESLIGHT_VISIBLE_CAMERA, false);
	data.SetBool(CYCLESLIGHT_VISIBLE_DIFFUSE, true);
	data.SetBool(CYCLESLIGHT_VISIBLE_GLOSSY, true);
	data.SetBool(CYCLESLIGHT_VISIBLE_TRANSMISSION, true);
	data.SetBool(CYCLESLIGHT_VISIBLE_VOLUME_SCATTER, true);

	data.SetFloat(CYCLESLIGHT_EEVEE_DIFFUSE_FACTOR, 1.0);
	data.SetFloat(CYCLESLIGHT_EEVEE_SPECULAR_FACTOR, 1.0);
	data.SetFloat(CYCLESLIGHT_EEVEE_TRANSMISSION_FACTOR, 1.0);
	data.SetFloat(CYCLESLIGHT_EEVEE_VOLUME_FACTOR, 1.0);
	data.SetBool(CYCLESLIGHT_EEVEE_USE_CUSTOM_DISTANCE, false);
	data.SetFloat(CYCLESLIGHT_EEVEE_CUSTOM_DISTANCE, LIGHTSETTINGS_DEFAULT::CUSTOM_DISTANCE);
	data.SetBool(CYCLESLIGHT_EEVEE_SHADOW_JITTER, false);
	data.SetFloat(CYCLESLIGHT_EEVEE_SHADOW_OVERBLUR, LIGHTSETTINGS_DEFAULT::SHADOW_OVERBLUR);
	data.SetFloat(CYCLESLIGHT_EEVEE_SHADOW_FILTER, LIGHTSETTINGS_DEFAULT::SHADOW_FILTER);
	data.SetFloat(CYCLESLIGHT_EEVEE_SHADOW_RESOLUTION, LIGHTSETTINGS_DEFAULT::SHADOW_RESOLUTION);
	data.SetBool(CYCLESLIGHT_EEVEE_SHADOW_ABSOLUTE, false);
}

static cinema::BaseContainer* Parameter(cinema::Description* description, maxon::Int32 id)
{
	return description->GetParameterI(CreateDescID(cinema::DescLevel(id)), nullptr);
}

static void SetHidden(cinema::Description* description, maxon::Int32 id, maxon::Bool hidden)
{
	if (cinema::BaseContainer* const parameter = Parameter(description, id))
		parameter->SetBool(cinema::DESC_HIDE, hidden);
}

static void SetName(cinema::Description* description, maxon::Int32 id, const maxon::String& name)
{
	if (cinema::BaseContainer* const parameter = Parameter(description, id))
	{
		parameter->SetString(cinema::DESC_NAME, name);
		parameter->SetString(cinema::DESC_SHORT_NAME, name);
	}
}

void ShowLightSettings(const cinema::BaseContainer& data, cinema::Description* description)
{
	const maxon::Int32 type = data.GetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT);
	const maxon::Int32 shape = data.GetInt32(CYCLESLIGHT_SHAPE, CYCLESLIGHT_SHAPE_SQUARE);
	const maxon::Bool point = type == CYCLESLIGHT_TYPE_POINT;
	const maxon::Bool sun = type == CYCLESLIGHT_TYPE_SUN;
	const maxon::Bool spot = type == CYCLESLIGHT_TYPE_SPOT;
	const maxon::Bool area = type == CYCLESLIGHT_TYPE_AREA;
	const maxon::Bool twoSizes = shape == CYCLESLIGHT_SHAPE_RECTANGLE || shape == CYCLESLIGHT_SHAPE_ELLIPSE;

	// The Light panel.
	SetHidden(description, CYCLESLIGHT_RADIUS, !(point || spot));
	SetHidden(description, CYCLESLIGHT_SOFT_FALLOFF, !(point || spot));
	SetHidden(description, CYCLESLIGHT_ANGLE, !sun);
	SetHidden(description, CYCLESLIGHT_SHAPE, !area);
	SetHidden(description, CYCLESLIGHT_SIZE, !area);
	SetHidden(description, CYCLESLIGHT_SIZE_Y, !(area && twoSizes));
	SetName(description, CYCLESLIGHT_SIZE, twoSizes ? "Size X"_s : "Size"_s);
	SetName(description, CYCLESLIGHT_ENERGY, sun ? "Strength (W/m2)"_s : "Power (W)"_s);
	SetName(description, CYCLESLIGHT_COLOR, data.GetBool(CYCLESLIGHT_USE_TEMPERATURE, false) ? "Tint"_s : "Color"_s);

	// Beam Shape: a spot's cone, an area's spread.
	SetHidden(description, CYCLESLIGHT_GROUP_BEAM, !(spot || area));
	SetHidden(description, CYCLESLIGHT_SPOT_SIZE, !spot);
	SetHidden(description, CYCLESLIGHT_SPOT_BLEND, !spot);
	SetHidden(description, CYCLESLIGHT_SHOW_CONE, !spot);
	SetHidden(description, CYCLESLIGHT_SPREAD, !area);

	// Settings: a portal is an area light only, and has no bounces.
	const maxon::Bool portal = area && data.GetBool(CYCLESLIGHT_PORTAL, false);
	SetHidden(description, CYCLESLIGHT_PORTAL, !area);
	SetHidden(description, CYCLESLIGHT_MAX_BOUNCES, portal);

	// Eevee: a sun has no custom distance and no absolute resolution limit.
	SetHidden(description, CYCLESLIGHT_GROUP_EEVEE_DISTANCE, sun);
	SetHidden(description, CYCLESLIGHT_EEVEE_SHADOW_ABSOLUTE, sun);
}

maxon::Bool IsLightSettingActive(const cinema::BaseContainer& data, maxon::Int32 id)
{
	const maxon::Bool portal = data.GetInt32(CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT) == CYCLESLIGHT_TYPE_AREA && data.GetBool(CYCLESLIGHT_PORTAL, false);
	switch (id)
	{
		case CYCLESLIGHT_TEMPERATURE: return data.GetBool(CYCLESLIGHT_USE_TEMPERATURE, false);
		case CYCLESLIGHT_EEVEE_SHADOW_OVERBLUR: return data.GetBool(CYCLESLIGHT_EEVEE_SHADOW_JITTER, false);
		case CYCLESLIGHT_EEVEE_CUSTOM_DISTANCE: return data.GetBool(CYCLESLIGHT_EEVEE_USE_CUSTOM_DISTANCE, false);
		case CYCLESLIGHT_CAST_SHADOW:
		case CYCLESLIGHT_MULTIPLE_IMPORTANCE:
		case CYCLESLIGHT_SHADOW_CAUSTICS: return !portal;
	}
	return true;
}

void ReadLightPanel(const cinema::BaseList2D* node, const Coords& coords, const DocumentColors& colors, BlenderLight& out)
{
	switch (GetIntParameter(node, CYCLESLIGHT_TYPE, CYCLESLIGHT_TYPE_POINT))
	{
		case CYCLESLIGHT_TYPE_SUN: out.type = BLENDERLIGHT::SUN; break;
		case CYCLESLIGHT_TYPE_SPOT: out.type = BLENDERLIGHT::SPOT; break;
		case CYCLESLIGHT_TYPE_AREA: out.type = BLENDERLIGHT::AREA; break;
		default: out.type = BLENDERLIGHT::POINT; break;
	}
	out.useTemperature = GetBoolParameter(node, CYCLESLIGHT_USE_TEMPERATURE, false);
	out.temperature = GetFloatParameter(node, CYCLESLIGHT_TEMPERATURE, LIGHTSETTINGS_DEFAULT::TEMPERATURE);
	out.color = colors.Input(GetVectorParameter(node, CYCLESLIGHT_COLOR, cinema::Vector(1.0)));
	out.energy = GetFloatParameter(node, CYCLESLIGHT_ENERGY, LIGHTSETTINGS_DEFAULT::ENERGY);
	out.exposure = GetFloatParameter(node, CYCLESLIGHT_EXPOSURE, 0.0);
	out.normalize = GetBoolParameter(node, CYCLESLIGHT_NORMALIZE, true);
	out.radius = coords.Length(maxon::Max(0.0, GetFloatParameter(node, CYCLESLIGHT_RADIUS, 0.0)));
	out.softFalloff = GetBoolParameter(node, CYCLESLIGHT_SOFT_FALLOFF, true);
	out.angle = GetFloatParameter(node, CYCLESLIGHT_ANGLE, LIGHTSETTINGS_DEFAULT::ANGLE);
	switch (GetIntParameter(node, CYCLESLIGHT_SHAPE, CYCLESLIGHT_SHAPE_SQUARE))
	{
		case CYCLESLIGHT_SHAPE_RECTANGLE: out.shape = BLENDERAREA::RECTANGLE; break;
		case CYCLESLIGHT_SHAPE_DISK: out.shape = BLENDERAREA::DISK; break;
		case CYCLESLIGHT_SHAPE_ELLIPSE: out.shape = BLENDERAREA::ELLIPSE; break;
		default: out.shape = BLENDERAREA::SQUARE; break;
	}
	out.size = coords.Length(maxon::Max(0.0, GetFloatParameter(node, CYCLESLIGHT_SIZE, LIGHTSETTINGS_DEFAULT::SIZE)));
	out.sizeY = coords.Length(maxon::Max(0.0, GetFloatParameter(node, CYCLESLIGHT_SIZE_Y, LIGHTSETTINGS_DEFAULT::SIZE)));
	out.spotSize = GetFloatParameter(node, CYCLESLIGHT_SPOT_SIZE, LIGHTSETTINGS_DEFAULT::SPOT_SIZE);
	out.spotBlend = GetFloatParameter(node, CYCLESLIGHT_SPOT_BLEND, LIGHTSETTINGS_DEFAULT::SPOT_BLEND);
	out.spread = GetFloatParameter(node, CYCLESLIGHT_SPREAD, LIGHTSETTINGS_DEFAULT::SPREAD);
	out.blenderSettings = true;
}

void ReadCyclesSettings(const cinema::BaseList2D* node, BlenderLight& out)
{
	out.maxBounces = GetIntParameter(node, CYCLESLIGHT_MAX_BOUNCES, LIGHTSETTINGS_DEFAULT::MAX_BOUNCES);
	out.useShadow = GetBoolParameter(node, CYCLESLIGHT_CAST_SHADOW, true);
	out.multipleImportance = GetBoolParameter(node, CYCLESLIGHT_MULTIPLE_IMPORTANCE, true);
	out.shadowCaustics = GetBoolParameter(node, CYCLESLIGHT_SHADOW_CAUSTICS, false);
	out.portal = GetBoolParameter(node, CYCLESLIGHT_PORTAL, false);
	out.cyclesSettings = true;
}

void ReadRayVisibility(const cinema::BaseList2D* node, BlenderLight& out)
{
	out.visibleCamera = GetBoolParameter(node, CYCLESLIGHT_VISIBLE_CAMERA, false);
	out.visibleDiffuse = GetBoolParameter(node, CYCLESLIGHT_VISIBLE_DIFFUSE, true);
	out.visibleGlossy = GetBoolParameter(node, CYCLESLIGHT_VISIBLE_GLOSSY, true);
	out.visibleTransmission = GetBoolParameter(node, CYCLESLIGHT_VISIBLE_TRANSMISSION, true);
	out.visibleVolumeScatter = GetBoolParameter(node, CYCLESLIGHT_VISIBLE_VOLUME_SCATTER, true);
	out.rayVisibility = true;
}

void ReadEeveeSettings(const cinema::BaseList2D* node, const Coords& coords, BlenderLight& out)
{
	out.diffuseFactor = GetFloatParameter(node, CYCLESLIGHT_EEVEE_DIFFUSE_FACTOR, 1.0);
	out.specularFactor = GetFloatParameter(node, CYCLESLIGHT_EEVEE_SPECULAR_FACTOR, 1.0);
	out.transmissionFactor = GetFloatParameter(node, CYCLESLIGHT_EEVEE_TRANSMISSION_FACTOR, 1.0);
	out.volumeFactor = GetFloatParameter(node, CYCLESLIGHT_EEVEE_VOLUME_FACTOR, 1.0);
	out.useCustomDistance = GetBoolParameter(node, CYCLESLIGHT_EEVEE_USE_CUSTOM_DISTANCE, false);
	out.customDistance = coords.Length(GetFloatParameter(node, CYCLESLIGHT_EEVEE_CUSTOM_DISTANCE, LIGHTSETTINGS_DEFAULT::CUSTOM_DISTANCE));
	out.shadowJitter = GetBoolParameter(node, CYCLESLIGHT_EEVEE_SHADOW_JITTER, false);
	// Cinema 4D keeps a percentage as a fraction; Blender's overblur is a number of percent.
	out.shadowOverblur = 100.0 * GetFloatParameter(node, CYCLESLIGHT_EEVEE_SHADOW_OVERBLUR, LIGHTSETTINGS_DEFAULT::SHADOW_OVERBLUR);
	out.shadowFilter = GetFloatParameter(node, CYCLESLIGHT_EEVEE_SHADOW_FILTER, LIGHTSETTINGS_DEFAULT::SHADOW_FILTER);
	out.shadowResolution = coords.Length(GetFloatParameter(node, CYCLESLIGHT_EEVEE_SHADOW_RESOLUTION, LIGHTSETTINGS_DEFAULT::SHADOW_RESOLUTION));
	out.shadowAbsolute = GetBoolParameter(node, CYCLESLIGHT_EEVEE_SHADOW_ABSOLUTE, false);
	out.eeveeSettings = true;
}

void StoreLightPanel(cinema::BaseContainer& data, const BlenderLight& values, const Coords& coords)
{
	const maxon::Float64 scale = coords.GetScale() > 0.0 ? coords.GetScale() : 0.01;
	maxon::Int32 type = CYCLESLIGHT_TYPE_POINT;
	switch (values.type)
	{
		case BLENDERLIGHT::SUN: type = CYCLESLIGHT_TYPE_SUN; break;
		case BLENDERLIGHT::SPOT: type = CYCLESLIGHT_TYPE_SPOT; break;
		case BLENDERLIGHT::AREA: type = CYCLESLIGHT_TYPE_AREA; break;
		case BLENDERLIGHT::POINT: break;
	}
	maxon::Int32 shape = CYCLESLIGHT_SHAPE_SQUARE;
	switch (values.shape)
	{
		case BLENDERAREA::RECTANGLE: shape = CYCLESLIGHT_SHAPE_RECTANGLE; break;
		case BLENDERAREA::DISK: shape = CYCLESLIGHT_SHAPE_DISK; break;
		case BLENDERAREA::ELLIPSE: shape = CYCLESLIGHT_SHAPE_ELLIPSE; break;
		case BLENDERAREA::SQUARE: break;
	}
	data.SetInt32(CYCLESLIGHT_TYPE, type);
	data.SetBool(CYCLESLIGHT_USE_TEMPERATURE, values.useTemperature);
	data.SetFloat(CYCLESLIGHT_TEMPERATURE, values.temperature);
	data.SetVector(CYCLESLIGHT_COLOR, values.color);
	data.SetFloat(CYCLESLIGHT_ENERGY, values.energy);
	data.SetFloat(CYCLESLIGHT_EXPOSURE, values.exposure);
	data.SetBool(CYCLESLIGHT_NORMALIZE, values.normalize);
	data.SetFloat(CYCLESLIGHT_RADIUS, values.radius / scale);
	data.SetBool(CYCLESLIGHT_SOFT_FALLOFF, values.softFalloff);
	data.SetFloat(CYCLESLIGHT_ANGLE, values.angle);
	data.SetInt32(CYCLESLIGHT_SHAPE, shape);
	data.SetFloat(CYCLESLIGHT_SIZE, values.size / scale);
	data.SetFloat(CYCLESLIGHT_SIZE_Y, values.sizeY / scale);
	data.SetFloat(CYCLESLIGHT_SPOT_SIZE, values.spotSize);
	data.SetFloat(CYCLESLIGHT_SPOT_BLEND, values.spotBlend);
	data.SetFloat(CYCLESLIGHT_SPREAD, values.spread);
}

} // namespace cyclesrender
