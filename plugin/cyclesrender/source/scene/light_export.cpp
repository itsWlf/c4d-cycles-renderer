// SPDX-License-Identifier: Apache-2.0
// Lights as `light` items (light_export.h): Cinema 4D's standard lights through measured
// conversion constants, Redshift lights exactly, the Cycles Light object and tag as they are.
// Reading order: the translation of each kind into a BlenderLight, then ExportLight, which picks
// the source and writes the item (WriteLight).

#include "c4d.h"
#include "olight.h"
#include "orslight.h"

#include "ocycleslight.h"
#include "tcycleslight.h"

#include "../ids.h"
#include "../lights/light_settings.h"
#include "../lights/light_tag.h"
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

static void Warn(SceneWriter* writer, const maxon::String& message)
{
	if (writer)
		writer->Warn(message);
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
static maxon::Bool TranslateRedshiftLight(const cinema::BaseObject* light, const Coords& coords, maxon::Float64 multiplier, BlenderLight& out,
	SceneWriter* writer)
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
		Warn(writer, FormatString("Light \"@\": Redshift portal lights are not translated; Cycles samples the environment without them.", light->GetName()));
		return false;
	}
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_UNITSTYPE, REDSHIFT_LIGHT_UNITSTYPE_IMAGE) != REDSHIFT_LIGHT_UNITSTYPE_IMAGE)
		Warn(writer, FormatString("Light \"@\": only the intensity unit \"Image\" is translated; its intensity is read as such.", light->GetName()));
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_DECAYTYPE, REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE) != REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE)
		Warn(writer, FormatString("Light \"@\": lights in Blender always fall off with the square of the distance.", light->GetName()));
	if (GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLORMODE, REDSHIFT_LIGHT_COLORMODE_COLOR) != REDSHIFT_LIGHT_COLORMODE_COLOR)
		Warn(writer, FormatString("Light \"@\": the colour temperature is not translated; the colour of the light is used.", light->GetName()));

	out.color = GetVectorParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLOR, cinema::Vector(1.0));
	out.useShadow = true;
	switch (type)
	{
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_INFINITE:
		case REDSHIFT_LIGHT_TYPE_PHYSICALSUN:
		{
			if (type == REDSHIFT_LIGHT_TYPE_PHYSICALSUN)
				Warn(writer, FormatString("Light \"@\": the Redshift sun is rendered as a plain infinite light.", light->GetName()));
			out.type = BLENDERLIGHT::SUN;
			out.energy = value;
			out.angle = 0.0;
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_SPOT:
		{
			const maxon::Float64 cone = maxon::ClampValue(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_ANGLE, maxon::PI * 40.0 / 180.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 falloff = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_FALLOFF_ANGLE, 0.0);
			out.type = BLENDERLIGHT::SPOT;
			out.energy = 4.0 * maxon::PI * value;
			out.radius = 0.0;
			out.spotSize = cone;
			out.spotBlend = SpotBlend(maxon::Max(0.0, cone - 2.0 * falloff), cone);
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_AREA:
		{
			const maxon::Int32 geometry = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_GEOMETRY, REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE);
			const maxon::Float64 sizeX = coords.Length(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEX, 200.0));
			const maxon::Float64 sizeY = coords.Length(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEY, 200.0));
			const maxon::Bool disc = geometry == REDSHIFT_LIGHT_AREA_GEOMETRY_DISC;
			if (!disc && geometry != REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE)
				Warn(writer, FormatString("Light \"@\": this area shape is rendered as a rectangle, and its brightness will differ.", light->GetName()));
			const maxon::Float64 area = disc ? maxon::PI * 0.25 * sizeX * sizeX : sizeX * sizeY;
			const maxon::Bool normalized = GetBoolParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_NORMALIZE, false);
			out.type = BLENDERLIGHT::AREA;
			out.energy = maxon::PI * value * (normalized ? 1.0 : area);
			out.shape = disc ? BLENDERAREA::DISK : BLENDERAREA::RECTANGLE;
			out.size = sizeX;
			out.sizeY = sizeY;
			break;
		}
		default:
		{
			if (type != REDSHIFT_LIGHT_TYPE_PHYSICAL_POINT)
				Warn(writer, FormatString("Light \"@\": this Redshift light type is rendered as a point light.", light->GetName()));
			out.type = BLENDERLIGHT::POINT;
			out.energy = 4.0 * maxon::PI * value;
			out.radius = 0.0;
			break;
		}
	}
	return true;
}

// A standard light (light_export.h, TranslateLight).
static maxon::Bool TranslateStandardLight(const cinema::BaseObject* light, const Coords& coords, const DocumentColors& colors, maxon::Float64 multiplier,
	BlenderLight& out, SceneWriter* writer)
{
	if (GetBoolParameter(light, LIGHT_NOLIGHTRADIATION, false))
		return false;
	if (GetBoolParameter(light, LIGHT_DETAILS_AMBIENT, false))
	{
		Warn(writer, FormatString("Light \"@\": ambient illumination has no counterpart in Blender; the light is skipped.", light->GetName()));
		return false;
	}

	const maxon::Float64 brightness = GetFloatParameter(light, LIGHT_BRIGHTNESS, 1.0) * multiplier;
	if (brightness <= 0.0)
		return false;
	out.color = colors.Input(GetVectorParameter(light, LIGHT_COLOR, cinema::Vector(1.0)));
	out.useShadow = GetIntParameter(light, LIGHT_SHADOWTYPE, LIGHT_SHADOWTYPE_NONE) != LIGHT_SHADOWTYPE_NONE;

	// The distance, in metres, at which the Blender light is as bright as the Cinema 4D light.
	const maxon::Float64 reference = maxon::Max(coords.Length(GetFloatParameter(light, LIGHT_DETAILS_OUTERDISTANCE, 500.0)), 1e-4);

	const maxon::Int32 type = GetIntParameter(light, LIGHT_TYPE, LIGHT_TYPE_OMNI);
	BLENDERLIGHT kind = BLENDERLIGHT::POINT;
	switch (type)
	{
		case LIGHT_TYPE_OMNI:
			break;
		case LIGHT_TYPE_SPOT:
			kind = BLENDERLIGHT::SPOT;
			break;
		case LIGHT_TYPE_SPOTRECT:
			kind = BLENDERLIGHT::SPOT;
			Warn(writer, FormatString("Light \"@\": a square spot is rendered as a round spot.", light->GetName()));
			break;
		case LIGHT_TYPE_DISTANT:
			kind = BLENDERLIGHT::SUN;
			break;
		case LIGHT_TYPE_PARALLEL:
		case LIGHT_TYPE_PARSPOT:
		case LIGHT_TYPE_PARSPOTRECT:
			kind = BLENDERLIGHT::SUN;
			Warn(writer, FormatString("Light \"@\": parallel lights are rendered as an infinite light, without their origin plane or spot shape.", light->GetName()));
			break;
		case LIGHT_TYPE_AREA:
			kind = BLENDERLIGHT::AREA;
			break;
		default:
			Warn(writer, FormatString("Light \"@\": this light type is not supported yet and is rendered as an omni light.", light->GetName()));
			break;
	}

	// An area light shaped as a sphere is what Blender calls a point light with a radius.
	const maxon::Int32 shape = GetIntParameter(light, LIGHT_AREADETAILS_SHAPE, LIGHT_AREADETAILS_SHAPE_RECTANGLE);
	const maxon::Float64 sizeX = coords.Length(GetFloatParameter(light, LIGHT_AREADETAILS_SIZEX, 200.0));
	const maxon::Float64 sizeY = coords.Length(GetFloatParameter(light, LIGHT_AREADETAILS_SIZEY, 200.0));
	const maxon::Bool sphere = kind == BLENDERLIGHT::AREA && shape == LIGHT_AREADETAILS_SHAPE_SPHERE;
	if (sphere)
		kind = BLENDERLIGHT::POINT;

	out.type = kind;
	switch (kind)
	{
		case BLENDERLIGHT::SUN:
			out.energy = brightness * SUN_STRENGTH;
			out.angle = 0.0;
			break;
		case BLENDERLIGHT::POINT:
			out.energy = brightness * POINT_POWER * reference * reference;
			out.radius = sphere ? sizeX * 0.5 : 0.0;
			break;
		case BLENDERLIGHT::SPOT:
		{
			const maxon::Float64 outer = maxon::ClampValue(GetFloatParameter(light, LIGHT_DETAILS_OUTERANGLE, maxon::PI / 6.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 inner = GetFloatParameter(light, LIGHT_DETAILS_INNERANGLE, 0.0);
			out.energy = brightness * POINT_POWER * reference * reference;
			out.radius = 0.0;
			out.spotSize = outer;
			out.spotBlend = SpotBlend(inner, outer);
			break;
		}
		case BLENDERLIGHT::AREA:
		{
			out.energy = brightness * AREA_POWER * reference * reference;
			out.size = sizeX;
			out.sizeY = sizeY;
			if (shape == LIGHT_AREADETAILS_SHAPE_DISC)
			{
				out.shape = BLENDERAREA::DISK;
			}
			else
			{
				if (shape != LIGHT_AREADETAILS_SHAPE_RECTANGLE)
					Warn(writer, FormatString("Light \"@\": this area shape is rendered as a rectangle.", light->GetName()));
				out.shape = BLENDERAREA::RECTANGLE;
			}
			break;
		}
	}
	return true;
}

maxon::Bool TranslateLight(const cinema::BaseObject* light, const Coords& coords, const DocumentColors& colors, maxon::Float64 multiplier,
	BlenderLight& out, SceneWriter* writer)
{
	if (!light)
		return false;
	if (light->GetType() == Orslight)
		return TranslateRedshiftLight(light, coords, multiplier, out, writer);
	return TranslateStandardLight(light, coords, colors, multiplier, out, writer);
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

// Writes the item. A translated light sends what it sent before the plugin had lights of its own,
// field for field; the extra blocks only when they were filled.
static void WriteLight(SceneWriter& writer, const Coords& coords, const cinema::Matrix& mg, const maxon::String& id, const BlenderLight& light,
	const LightLinking* linking)
{
	maxon::Float64 matrix[16];
	coords.AimedMatrix(mg, matrix);
	const maxon::Float64 rgb[3] = { light.color.x, light.color.y, light.color.z };

	JsonWriter& json = writer.BeginItem("light", id);
	json.Floats("matrix", matrix, 16);
	json.Floats("color", rgb, 3);
	json.Bool("use_shadow", light.useShadow);
	WriteLinking(json, linking);
	switch (light.type)
	{
		case BLENDERLIGHT::SUN:
			json.String("type", "SUN"_s);
			json.Float("energy", light.energy);
			json.Float("angle", light.angle);
			break;
		case BLENDERLIGHT::POINT:
			json.String("type", "POINT"_s);
			json.Float("energy", light.energy);
			json.Float("shadow_soft_size", light.radius);
			break;
		case BLENDERLIGHT::SPOT:
			json.String("type", "SPOT"_s);
			json.Float("energy", light.energy);
			json.Float("shadow_soft_size", light.radius);
			json.Float("spot_size", maxon::ClampValue(light.spotSize, MIN_SPOT_SIZE, MAX_SPOT_SIZE));
			json.Float("spot_blend", maxon::Clamp01(light.spotBlend));
			break;
		case BLENDERLIGHT::AREA:
		{
			json.String("type", "AREA"_s);
			json.Float("energy", light.energy);
			static const maxon::Char* const SHAPES[] = { "SQUARE", "RECTANGLE", "DISK", "ELLIPSE" };
			json.String("shape", maxon::String(SHAPES[maxon::Int(light.shape)]));
			json.Float("size", light.size);
			if (light.shape == BLENDERAREA::RECTANGLE || light.shape == BLENDERAREA::ELLIPSE)
				json.Float("size_y", light.sizeY);
			break;
		}
	}
	if (light.blenderSettings)
	{
		json.Float("exposure", light.exposure);
		json.Bool("normalize", light.normalize);
		json.Bool("use_temperature", light.useTemperature);
		json.Float("temperature", light.temperature);
		if (light.type == BLENDERLIGHT::POINT || light.type == BLENDERLIGHT::SPOT)
			json.Bool("use_soft_falloff", light.softFalloff);
		if (light.type == BLENDERLIGHT::AREA)
			json.Float("spread", light.spread);
	}
	if (light.cyclesSettings)
	{
		json.BeginObject("cycles");
		json.Int("max_bounces", light.maxBounces);
		json.Bool("use_multiple_importance_sampling", light.multipleImportance);
		json.Bool("is_caustics_light", light.shadowCaustics);
		json.Bool("is_portal", light.type == BLENDERLIGHT::AREA && light.portal);
		json.EndObject();
	}
	if (light.rayVisibility)
	{
		json.BeginObject("visibility");
		json.Bool("camera", light.visibleCamera);
		json.Bool("diffuse", light.visibleDiffuse);
		json.Bool("glossy", light.visibleGlossy);
		json.Bool("transmission", light.visibleTransmission);
		json.Bool("volume_scatter", light.visibleVolumeScatter);
		json.EndObject();
	}
	if (light.eeveeSettings)
	{
		json.Float("diffuse_factor", light.diffuseFactor);
		json.Float("specular_factor", light.specularFactor);
		json.Float("transmission_factor", light.transmissionFactor);
		json.Float("volume_factor", light.volumeFactor);
		json.Bool("use_shadow_jitter", light.shadowJitter);
		json.Float("shadow_jitter_overblur", light.shadowOverblur);
		json.Float("shadow_filter_radius", light.shadowFilter);
		json.Float("shadow_maximum_resolution", light.shadowResolution);
		if (light.type != BLENDERLIGHT::SUN)
		{
			json.Bool("use_custom_distance", light.useCustomDistance);
			json.Float("cutoff_distance", light.customDistance);
			json.Bool("use_absolute_resolution", light.shadowAbsolute);
		}
	}
	writer.EndItem();
}

maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking)
{
	BlenderLight values;
	if (light->GetType() == ID_LIGHT_OBJECT)
	{
		// The plugin's own light: Blender's values, every tab.
		ReadLightPanel(light, coords, colors, values);
		ReadCyclesSettings(light, values);
		ReadRayVisibility(light, values);
		ReadEeveeSettings(light, coords, values);
		values.energy *= multiplier;
		WriteLight(writer, coords, mg, id, values, linking);
		return true;
	}

	// One of Cinema 4D's lights, perhaps with a Cycles Light tag. Its Light tab replaces what the
	// light emits; the light's own switch "No Light Radiation" still turns it off.
	const cinema::BaseTag* const tag = FindLightTag(light);
	if (tag && GetBoolParameter(tag, CYCLESLIGHTTAG_LIGHT_ENABLE, false))
	{
		if (light->GetType() == Olight && GetBoolParameter(light, LIGHT_NOLIGHTRADIATION, false))
			return false;
		// The light's own shadow setting stays until the Settings tab says otherwise.
		const maxon::Bool shadow = light->GetType() == Orslight ||
			GetIntParameter(light, LIGHT_SHADOWTYPE, LIGHT_SHADOWTYPE_NONE) != LIGHT_SHADOWTYPE_NONE;
		ReadLightPanel(tag, coords, colors, values);
		values.useShadow = shadow;
		values.energy *= multiplier;
	}
	else if (!TranslateLight(light, coords, colors, multiplier, values, &writer))
	{
		return false;
	}
	if (tag)
	{
		if (GetBoolParameter(tag, CYCLESLIGHTTAG_SETTINGS_ENABLE, false))
			ReadCyclesSettings(tag, values);
		if (GetBoolParameter(tag, CYCLESLIGHTTAG_VISIBILITY_ENABLE, false))
			ReadRayVisibility(tag, values);
		if (GetBoolParameter(tag, CYCLESLIGHTTAG_EEVEE_ENABLE, false))
			ReadEeveeSettings(tag, coords, values);
	}
	WriteLight(writer, coords, mg, id, values, linking);
	return true;
}

} // namespace cyclesrender
