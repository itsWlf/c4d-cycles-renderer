// SPDX-License-Identifier: Apache-2.0
// Lights as `light` items (light_export.h): Cinema 4D's standard lights through measured
// conversion constants, Redshift lights exactly.

#include "c4d.h"
#include "olight.h"
#include "orslight.h"

#include "light_export.h"
#include "parameters.h"

namespace cyclesrender
{

// Blender power that makes a white diffuse surface render as 1.0, measured with
// tests/bridge/measure_lights.py on Blender 5.2.3:
//   sun:          strength = pi                    (W/m2, at any distance)
//   point, spot:  power = 4 * pi^2 * distance^2    (W, distance in metres)
//   area:         power = pi^2 * distance^2        (W, on the light's axis, far from it)
static const maxon::Float64 SUN_STRENGTH = maxon::PI;
static const maxon::Float64 POINT_POWER = 4.0 * maxon::PI * maxon::PI;
static const maxon::Float64 AREA_POWER = maxon::PI * maxon::PI;

// Blender's limits for a spot cone, in radians.
static const maxon::Float64 MIN_SPOT_SIZE = maxon::PI / 180.0;
static const maxon::Float64 MAX_SPOT_SIZE = maxon::PI;

enum class LIGHTKIND
{
	POINT,
	SUN,
	SPOT,
	AREA
} MAXON_ENUM_LIST(LIGHTKIND);

// Blender's spot blend is the part of the cone, measured in the cosine of the angle from the
// axis, over which the light fades out. This picks the blend that starts fading at Cinema 4D's
// inner angle. Both angles are full cone angles in radians.
static maxon::Float64 SpotBlend(maxon::Float64 innerAngle, maxon::Float64 outerAngle)
{
	const maxon::Float64 cosOuter = maxon::Cos(outerAngle * 0.5);
	const maxon::Float64 cosInner = maxon::Cos(maxon::Min(innerAngle, outerAngle) * 0.5);
	const maxon::Float64 range = 1.0 - cosOuter;
	if (range < 1e-9)
		return 0.0;
	return maxon::Clamp01((cosInner - cosOuter) / range);
}

static void WriteLinking(JsonWriter& json, const LightLinking* linking)
{
	if (!linking)
		return;
	json.BeginObject("linking");
	json.String("mode", linking->include ? "INCLUDE"_s : "EXCLUDE"_s);
	json.BeginArray("objects");
	for (const maxon::String& itemId : linking->itemIds)
		json.String(nullptr, itemId);
	json.EndArray();
	json.EndObject();
}

//----------------------------------------------------------------------------------------
// A Redshift light. Unlike Cinema 4D's standard lights these are physical, so the conversion
// is exact. Measured by rendering a white wall with Redshift (docs/reference/c4d-sdk.md,
// "Redshift lights"), with intensity I, exposure e and v = I * 2^e, units "Image":
//   infinite:     the wall renders as v / pi                       -> sun strength v
//   point, spot:  v / (pi * d^2), d in metres                      -> power 4 * pi * v
//   area:         its radiance is v, or v / area with "Normalize"  -> power pi * v * area, or pi * v
// A spot's cone angle is the full angle; its falloff angle lies inside the cone. A disc's size
// is its diameter.
//----------------------------------------------------------------------------------------
static maxon::Result<maxon::Bool> ExportRedshiftLight(SceneWriter& writer, const Coords& coords, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking)
{
	const maxon::Float64 value = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_INTENSITY, 1.0) *
		maxon::Pow(2.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_EXPOSURE, 0.0)) * multiplier;
	if (value <= 0.0)
		return false;

	const maxon::Int32 type = GetIntParameter(light, REDSHIFT_LIGHT_TYPE, REDSHIFT_LIGHT_TYPE_PHYSICAL_POINT);
	if (type == REDSHIFT_LIGHT_TYPE_DOME)
		return false;   // the environment: scene_exporter.cpp hands it to ExportDomeWorld
	if (type == REDSHIFT_LIGHT_TYPE_PORTAL)
	{
		writer.Warn(FormatString("Light \"@\": Redshift portal lights are not translated; Cycles samples the environment without them.", light->GetName()));
		return false;
	}
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_UNITSTYPE, REDSHIFT_LIGHT_UNITSTYPE_IMAGE) != REDSHIFT_LIGHT_UNITSTYPE_IMAGE)
		writer.Warn(FormatString("Light \"@\": only the intensity unit \"Image\" is translated; its intensity is read as such.", light->GetName()));
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_DECAYTYPE, REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE) != REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE)
		writer.Warn(FormatString("Light \"@\": lights in Blender always fall off with the square of the distance.", light->GetName()));
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLORMODE, REDSHIFT_LIGHT_COLORMODE_COLOR) != REDSHIFT_LIGHT_COLORMODE_COLOR)
		writer.Warn(FormatString("Light \"@\": the colour temperature is not translated; the colour of the light is used.", light->GetName()));

	maxon::Float64 matrix[16];
	coords.AimedMatrix(mg, matrix);
	const cinema::Vector color = GetVectorParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLOR, cinema::Vector(1.0));
	const maxon::Float64 rgb[3] = { color.x, color.y, color.z };

	JsonWriter& json = writer.BeginItem("light", id);
	json.Floats("matrix", matrix, 16);
	json.Floats("color", rgb, 3);
	json.Bool("use_shadow", true);
	WriteLinking(json, linking);
	switch (type)
	{
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_INFINITE:
		case REDSHIFT_LIGHT_TYPE_PHYSICALSUN:
		{
			if (type == REDSHIFT_LIGHT_TYPE_PHYSICALSUN)
				writer.Warn(FormatString("Light \"@\": the Redshift sun is rendered as a plain infinite light.", light->GetName()));
			json.String("type", "SUN"_s);
			json.Float("energy", value);
			json.Float("angle", 0.0);
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_SPOT:
		{
			const maxon::Float64 cone = maxon::ClampValue(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_ANGLE, maxon::PI * 40.0 / 180.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 falloff = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_FALLOFF_ANGLE, 0.0);
			json.String("type", "SPOT"_s);
			json.Float("energy", 4.0 * maxon::PI * value);
			json.Float("shadow_soft_size", 0.0);
			json.Float("spot_size", cone);
			json.Float("spot_blend", SpotBlend(maxon::Max(0.0, cone - 2.0 * falloff), cone));
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_AREA:
		{
			const maxon::Int32 geometry = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_GEOMETRY, REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE);
			const maxon::Float64 sizeX = coords.Length(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEX, 200.0));
			const maxon::Float64 sizeY = coords.Length(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEY, 200.0));
			const maxon::Bool disc = geometry == REDSHIFT_LIGHT_AREA_GEOMETRY_DISC;
			if (!disc && geometry != REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE)
				writer.Warn(FormatString("Light \"@\": this area shape is rendered as a rectangle, and its brightness will differ.", light->GetName()));
			const maxon::Float64 area = disc ? maxon::PI * 0.25 * sizeX * sizeX : sizeX * sizeY;
			const maxon::Bool normalized = GetBoolParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_NORMALIZE, false);
			json.String("type", "AREA"_s);
			json.Float("energy", maxon::PI * value * (normalized ? 1.0 : area));
			json.String("shape", disc ? "DISK"_s : "RECTANGLE"_s);
			json.Float("size", sizeX);
			if (!disc)
				json.Float("size_y", sizeY);
			break;
		}
		default:
		{
			if (type != REDSHIFT_LIGHT_TYPE_PHYSICAL_POINT)
				writer.Warn(FormatString("Light \"@\": this Redshift light type is rendered as a point light.", light->GetName()));
			json.String("type", "POINT"_s);
			json.Float("energy", 4.0 * maxon::PI * value);
			json.Float("shadow_soft_size", 0.0);
			break;
		}
	}
	writer.EndItem();
	return true;
}

maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking)
{
	if (light->GetType() == Orslight)
		return ExportRedshiftLight(writer, coords, light, mg, id, multiplier, linking);

	if (GetBoolParameter(light, LIGHT_NOLIGHTRADIATION, false))
		return false;
	if (GetBoolParameter(light, LIGHT_DETAILS_AMBIENT, false))
	{
		writer.Warn(FormatString("Light \"@\": ambient illumination has no counterpart in Blender; the light is skipped.", light->GetName()));
		return false;
	}

	const maxon::Float64 brightness = GetFloatParameter(light, LIGHT_BRIGHTNESS, 1.0) * multiplier;
	const cinema::Vector color = GetVectorParameter(light, LIGHT_COLOR, cinema::Vector(1.0));
	if (brightness <= 0.0)
		return false;

	// The distance, in metres, at which the Blender light is as bright as the Cinema 4D light.
	const maxon::Float64 reference = maxon::Max(coords.Length(GetFloatParameter(light, LIGHT_DETAILS_OUTERDISTANCE, 500.0)), 1e-4);

	const maxon::Int32 type = GetIntParameter(light, LIGHT_TYPE, LIGHT_TYPE_OMNI);
	LIGHTKIND kind = LIGHTKIND::POINT;
	switch (type)
	{
		case LIGHT_TYPE_OMNI:
			break;
		case LIGHT_TYPE_SPOT:
			kind = LIGHTKIND::SPOT;
			break;
		case LIGHT_TYPE_SPOTRECT:
			kind = LIGHTKIND::SPOT;
			writer.Warn(FormatString("Light \"@\": a square spot is rendered as a round spot.", light->GetName()));
			break;
		case LIGHT_TYPE_DISTANT:
			kind = LIGHTKIND::SUN;
			break;
		case LIGHT_TYPE_PARALLEL:
		case LIGHT_TYPE_PARSPOT:
		case LIGHT_TYPE_PARSPOTRECT:
			kind = LIGHTKIND::SUN;
			writer.Warn(FormatString("Light \"@\": parallel lights are rendered as an infinite light, without their origin plane or spot shape.", light->GetName()));
			break;
		case LIGHT_TYPE_AREA:
			kind = LIGHTKIND::AREA;
			break;
		default:
			writer.Warn(FormatString("Light \"@\": this light type is not supported yet and is rendered as an omni light.", light->GetName()));
			break;
	}

	maxon::Float64 matrix[16];
	coords.AimedMatrix(mg, matrix);
	const maxon::Float64 rgb[3] = { color.x, color.y, color.z };

	// An area light shaped as a sphere is what Blender calls a point light with a radius.
	const maxon::Int32 shape = GetIntParameter(light, LIGHT_AREADETAILS_SHAPE, LIGHT_AREADETAILS_SHAPE_RECTANGLE);
	const maxon::Float64 sizeX = coords.Length(GetFloatParameter(light, LIGHT_AREADETAILS_SIZEX, 200.0));
	const maxon::Float64 sizeY = coords.Length(GetFloatParameter(light, LIGHT_AREADETAILS_SIZEY, 200.0));
	const maxon::Bool sphere = kind == LIGHTKIND::AREA && shape == LIGHT_AREADETAILS_SHAPE_SPHERE;
	if (sphere)
		kind = LIGHTKIND::POINT;

	JsonWriter& json = writer.BeginItem("light", id);
	json.Floats("matrix", matrix, 16);
	json.Floats("color", rgb, 3);
	json.Bool("use_shadow", GetIntParameter(light, LIGHT_SHADOWTYPE, LIGHT_SHADOWTYPE_NONE) != LIGHT_SHADOWTYPE_NONE);
	WriteLinking(json, linking);

	switch (kind)
	{
		case LIGHTKIND::SUN:
		{
			json.String("type", "SUN"_s);
			json.Float("energy", brightness * SUN_STRENGTH);
			json.Float("angle", 0.0);
			break;
		}
		case LIGHTKIND::POINT:
		{
			json.String("type", "POINT"_s);
			json.Float("energy", brightness * POINT_POWER * reference * reference);
			json.Float("shadow_soft_size", sphere ? sizeX * 0.5 : 0.0);
			break;
		}
		case LIGHTKIND::SPOT:
		{
			const maxon::Float64 outer = maxon::ClampValue(GetFloatParameter(light, LIGHT_DETAILS_OUTERANGLE, maxon::PI / 6.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 inner = GetFloatParameter(light, LIGHT_DETAILS_INNERANGLE, 0.0);
			json.String("type", "SPOT"_s);
			json.Float("energy", brightness * POINT_POWER * reference * reference);
			json.Float("shadow_soft_size", 0.0);
			json.Float("spot_size", outer);
			json.Float("spot_blend", SpotBlend(inner, outer));
			break;
		}
		case LIGHTKIND::AREA:
		{
			json.String("type", "AREA"_s);
			json.Float("energy", brightness * AREA_POWER * reference * reference);
			if (shape == LIGHT_AREADETAILS_SHAPE_DISC)
			{
				json.String("shape", "DISK"_s);
				json.Float("size", sizeX);
			}
			else
			{
				if (shape != LIGHT_AREADETAILS_SHAPE_RECTANGLE)
					writer.Warn(FormatString("Light \"@\": this area shape is rendered as a rectangle.", light->GetName()));
				json.String("shape", "RECTANGLE"_s);
				json.Float("size", sizeX);
				json.Float("size_y", sizeY);
			}
			break;
		}
	}
	writer.EndItem();
	return true;
}

} // namespace cyclesrender
