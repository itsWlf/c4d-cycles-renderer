// SPDX-License-Identifier: Apache-2.0
// Lights as `light` items (light_export.h): Cinema 4D's standard lights through measured
// conversion constants, Redshift lights exactly, the Cycles Light object and tag as they are.
// Reading order: the translation of each kind into a BlenderLight, then ExportLight, which picks
// the source and writes the item (WriteLight).

#include "c4d.h"
#include "drsfile.h"
#include "maxon/atomictypes.h"
#include "olight.h"
#include "orslight.h"

#include "ocycleslight.h"
#include "tcycleslight.h"

#include "../ids.h"
#include "../sdk_compat.h"
#include "../lights/light_settings.h"
#include "../lights/light_tag.h"
#include "light_export.h"
#include "parameters.h"
#include "redshift_ids.h"

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

// A Redshift light's own "Shadow" switch (on for a new light; Redshift renders none without it).
static maxon::Bool RedshiftLightCastsShadow(const cinema::BaseObject* light)
{
	return GetBoolParameter(light, REDSHIFT_LIGHT_SHADOW, true);
}

maxon::Bool IsLightSwitchedOn(const cinema::BaseObject* light)
{
	return light && light->GetDeformMode();
}

//----------------------------------------------------------------------------------------
// The softness of a standard light's shadow type in Blender's terms: the size of the light.
// Measured in the Standard renderer (docs/reference/c4d-sdk.md, "Soft
// and area shadows"): the 10-90 % width W of a straight shadow edge on a floor, the caster
// between light and floor, g = (light-to-floor - light-to-caster) / light-to-caster.
//
//   Blender, Cycles: a point or spot light of radius r: W = 1.391 r g; a sun of angle a
//                    (its angular diameter): W = 0.692 a gap (gap = caster to floor).
//   Area shadow, omni or spot: the Details tab's area shape is the source, a physical penumbra:
//                    rectangle of side s across the edge W = 0.795 s g; disc of diameter d
//                    W = 0.678 d g; sphere of diameter d W = 0.826 d g.
//   Area shadow, infinite light: W = 1.567 x gap, x its "Infinite Angle".
//   Shadow maps (Soft): a blur of a fixed angle seen from the light, whatever the caster:
//                    omni or spot W = 1.1 (R + 1) 2 tan(F / 2) / N x light-to-floor, with R the
//                    sample radius, N the map size, F the map's field (90 degrees for an omni,
//                    the cone for a spot or with "Shadow Cone"); infinite W = 1.1 (R + 1) P / N,
//                    P the parallel width.
//----------------------------------------------------------------------------------------
static const maxon::Float64 BLENDER_POINT_PENUMBRA = 1.391;
static const maxon::Float64 BLENDER_SUN_PENUMBRA = 0.692;
static const maxon::Float64 C4D_SPHERE_PENUMBRA = 0.826;
static const maxon::Float64 C4D_INFINITE_PENUMBRA = 1.567;
static const maxon::Float64 C4D_SHADOW_MAP_BLUR = 1.1;

// A shadow map's resolution: its texels across (the two sides averaged for a map of the user's size).
static maxon::Float64 ShadowMapTexels(const cinema::BaseObject* light)
{
	const maxon::Float64 x = maxon::Float64(GetIntParameter(light, LIGHT_SHADOW_MAPSIZEX, 250));
	const maxon::Float64 y = maxon::Float64(GetIntParameter(light, LIGHT_SHADOW_MAPSIZEY, 250));
	return maxon::Max(1.0, 0.5 * (x + y));
}

// The blur of a shadow map, in texels: its sample radius, plus one.
static maxon::Float64 ShadowMapBlur(const cinema::BaseObject* light)
{
	return C4D_SHADOW_MAP_BLUR * (maxon::Float64(maxon::Max(0, GetIntParameter(light, LIGHT_SHADOW_SAMPLERADIUS, 3))) + 1.0);
}

// Sets out.radius (point, spot) or out.angle (sun) from the light's shadow type. Raytraced (Hard)
// and None are sharp: 0. An area light, or an area light shaped as a sphere (a point light with
// its own radius), keeps its own size, whatever the shadow type: the caller does not ask.
// `reference` is the distance in metres at which the light's brightness matches (its Radius/Decay);
// a shadow map's blur, which is not physical, is matched for a caster halfway along it.
static void ApplyShadowSoftness(const cinema::BaseObject* light, const Coords& coords, maxon::Float64 reference, BlenderLight& out, SceneWriter* writer)
{
	const maxon::Int32 shadow = GetIntParameter(light, LIGHT_SHADOWTYPE, LIGHT_SHADOWTYPE_NONE);
	const maxon::Bool sun = out.type == BLENDERLIGHT::SUN;
	if (shadow == LIGHT_SHADOWTYPE_AREA)
	{
		if (sun)
		{
			// The same edge as Cinema 4D's for every gap: angle = 1.567 / 0.692 x Infinite Angle.
			const maxon::Float64 angle = GetFloatParameter(light, LIGHT_AREADETAILS_INFINITE_ANGLE, maxon::PI / 360.0);
			out.angle = maxon::ClampValue(angle * C4D_INFINITE_PENUMBRA / BLENDER_SUN_PENUMBRA, 0.0, maxon::PI);
			return;
		}
		// The area shape of the Details tab. A disc is Blender's sphere light seen from the side it
		// lights (radius = half the diameter, 2.5 % narrower than Cinema 4D's edge); Cinema 4D's sphere
		// casts a wider edge than a disc of its size (0.826 against 0.678), matched; a rectangle and
		// every other shape become the radius of a disc of the same area, which matches the default
		// 200 x 200 square within 1.3 % (a rectangle's edge depends on its side across the edge, a
		// sphere light's does not).
		const maxon::Int32 shape = GetIntParameter(light, LIGHT_AREADETAILS_SHAPE, LIGHT_AREADETAILS_SHAPE_RECTANGLE);
		const maxon::Float64 sizeX = coords.Length(maxon::Max(0.0, GetFloatParameter(light, LIGHT_AREADETAILS_SIZEX, 200.0)));
		const maxon::Float64 sizeY = coords.Length(maxon::Max(0.0, GetFloatParameter(light, LIGHT_AREADETAILS_SIZEY, 200.0)));
		switch (shape)
		{
			case LIGHT_AREADETAILS_SHAPE_DISC:
				out.radius = 0.5 * sizeX;
				break;
			case LIGHT_AREADETAILS_SHAPE_SPHERE:
			case LIGHT_AREADETAILS_SHAPE_HEMISPHERE:
				out.radius = sizeX * C4D_SPHERE_PENUMBRA / BLENDER_POINT_PENUMBRA;
				break;
			default:
				if (shape != LIGHT_AREADETAILS_SHAPE_RECTANGLE)
					Warn(writer, FormatString("Light \"@\": its area shadow's shape is rendered as a round light of the same area (Size X by Size Y).", light->GetName()));
				out.radius = maxon::Sqrt(sizeX * sizeY / maxon::PI);
				break;
		}
		return;
	}
	if (shadow == LIGHT_SHADOWTYPE_SOFT)
	{
		const maxon::Float64 blur = ShadowMapBlur(light) / ShadowMapTexels(light);
		if (sun)
		{
			// Cinema 4D's blur is a width (blur x parallel width), whatever the gap between caster and
			// floor; Blender's sun gives a width growing with the gap. Matched, as for an omni, with the
			// caster halfway along the light's reference distance (a gap of half of it).
			const maxon::Float64 width = blur * coords.Length(maxon::Max(0.0, GetFloatParameter(light, LIGHT_SHADOW_PARALLELWIDTH, 500.0)));
			out.angle = maxon::ClampValue(width / (BLENDER_SUN_PENUMBRA * 0.5 * reference), 0.0, maxon::PI);
			return;
		}
		// Cinema 4D's blur is an angle seen from the light; Blender's edge grows with the gap. Matched
		// at the light's reference distance with the caster halfway (g = 1).
		maxon::Float64 field = maxon::PI * 0.5;
		if (out.type == BLENDERLIGHT::SPOT)
			field = out.spotSize;
		if (GetBoolParameter(light, LIGHT_SHADOW_USECONE, false))
			field = GetFloatParameter(light, LIGHT_SHADOW_CONEANGLE, maxon::PI * 0.5);
		field = maxon::ClampValue(field, MIN_SPOT_SIZE, maxon::PI * 175.0 / 180.0);
		const maxon::Float64 angle = blur * 2.0 * maxon::Tan(field * 0.5);
		out.radius = angle * reference / BLENDER_POINT_PENUMBRA;
	}
}

//----------------------------------------------------------------------------------------
// A Redshift light. Unlike Cinema 4D's standard lights these are physical, so the conversion
// is exact where Blender has the same light. Measured with Redshift 2027.0.0 in Cinema 4D 2027.0.0
// (docs/reference/c4d-sdk.md, "Redshift lights"; tests/c4d/redshift_lights.py), with v = intensity
// * 2^exposure in the unit "Image" (the default):
//   infinite:     a white wall renders as v / pi                   -> sun strength v
//   point, spot:  its radiant intensity is v (W/sr)                -> power 4 * pi * v
//   area:         its radiance is v, or v / A with "Normalize", A the surface that emits
//                 (rectangle sx * sy, disc pi/4 sx sy, sphere pi d^2) -> power pi * L * A
// The other units are photometric, at 683 lm/W, or radiometric through the light's own
// "Luminous Efficacy" (RedshiftUnitFactor). The light's size counts with the object's scale.
// The colour modes "Temperature" and "Temperature and Color" use RedshiftTemperatureColor.
//----------------------------------------------------------------------------------------

// (Redshift's own ids that not every SDK's copy of orslight.h lists, the units "Luminous Intensity
// (cd)" and "Luminous Exitance (lm/m2)" and the ray contribution "Camera", are in redshift_ids.h.)

// The photometric constant (lm per W at 555 nm) Redshift converts every photometric unit with
// (measured: candela / 683 is the intensity of "Image"), whatever the light's own efficacy.
static const maxon::Float64 LUMENS_PER_WATT = 683.0;

// What a value in Redshift's unit `unit` is in the unit "Image", for a light whose "Image" value
// is a radiant intensity spread over the solid angle `solidAngle` (point 4 pi, spot its cone; a
// sun 4 pi, measured), or, for an area light (`surface` > 0), a radiance over `surface` square
// metres. `efficacy` is the light's "Luminous Efficacy (lm/W)", which only the radiometric units
// use (Redshift turns their watts into lumens with it). Measured with Redshift 2027.0.0: every
// factor below to 0.2 % on a floor lit by the light. Returns 0 for a unit Redshift renders black
// for this kind of light (the area units on a point or spot light: 1e-6 of the light).
static maxon::Float64 RedshiftUnitFactor(maxon::Int32 unit, maxon::Float64 efficacy, maxon::Float64 solidAngle, maxon::Float64 surface, maxon::Bool sun)
{
	const maxon::Bool area = surface > 0.0;
	switch (unit)
	{
		case REDSHIFT_LIGHT_UNITSTYPE_IMAGE:
			return 1.0;
		case REDSHIFT_LIGHT_UNITSTYPE_LUMINOUS_POWER_LM:
			return 1.0 / (LUMENS_PER_WATT * (area ? maxon::PI * surface : solidAngle));
		case REDSHIFT_LIGHT_UNITSTYPE_RADIANT_POWER_W:
			return efficacy / (LUMENS_PER_WATT * (area ? maxon::PI * surface : solidAngle));
		case REDSHIFT_UNITS_LUMINOUS_INTENSITY:
			return 1.0 / (LUMENS_PER_WATT * (area ? surface : 1.0));
		case REDSHIFT_LIGHT_UNITSTYPE_LUMINANCE_CD_PER_MSQUARED:
			return (area || sun) ? 1.0 / LUMENS_PER_WATT : 0.0;
		case REDSHIFT_LIGHT_UNITSTYPE_RADIANCE_W_PER_SR_PER_MSQUARED:
			return (area || sun) ? efficacy / LUMENS_PER_WATT : 0.0;
		case REDSHIFT_UNITS_LUMINOUS_EXITANCE:
			if (sun)
				return 1.0 / (LUMENS_PER_WATT * 4.0 * maxon::PI);
			return area ? 1.0 / (LUMENS_PER_WATT * maxon::PI) : 0.0;
		default:
			return 1.0;
	}
}

// The colour of Redshift's colour temperature `kelvin` in the space Blender calculates in, as
// Redshift computes it (measured, Redshift 2027.0.0, in ACEScg and in linear Rec.709 render spaces,
// 1000 to 25000 K, within 0.003): the chromaticity of the Planckian locus by Kim et al. (2002;
// Krystek's 1985 approximation below 1667 K, where Kim's does not hold), the temperature held to
// 1000 to 20000 K, made a colour of the render space (DocumentColors::FromXyz), negative parts
// cut off, divided by its largest part. Its brightness is not kept: 3000 K in ACEScg is
// (1, 0.65, 0.26). Redshift's 6500 K is not white (the locus misses D65): (0.995, 0.961, 1).
static cinema::Vector RedshiftTemperatureColor(maxon::Float64 kelvin, const DocumentColors& colors)
{
	const maxon::Float64 t = maxon::ClampValue(kelvin, 1000.0, 20000.0);
	maxon::Float64 x = 0.0;
	maxon::Float64 y = 0.0;
	if (t < 1667.0)
	{
		// Krystek: CIE 1960 (u, v), then (x, y).
		const maxon::Float64 u = (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) / (1.0 + 8.42420235e-4 * t + 7.08145163e-7 * t * t);
		const maxon::Float64 v = (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) / (1.0 - 2.89741816e-5 * t + 1.61456053e-7 * t * t);
		const maxon::Float64 d = 2.0 * u - 8.0 * v + 4.0;
		x = 3.0 * u / d;
		y = 2.0 * v / d;
	}
	else
	{
		const maxon::Float64 t2 = t * t;
		const maxon::Float64 t3 = t2 * t;
		if (t <= 4000.0)
			x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
		else
			x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
		const maxon::Float64 x2 = x * x;
		const maxon::Float64 x3 = x2 * x;
		if (t <= 2222.0)
			y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
		else if (t <= 4000.0)
			y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
		else
			y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
	}
	if (y <= 1e-9)
		return cinema::Vector(1.0);
	cinema::Vector rgb = colors.FromXyz(cinema::Vector(x / y, 1.0, (1.0 - x - y) / y));
	rgb = cinema::Vector(maxon::Max(0.0, rgb.x), maxon::Max(0.0, rgb.y), maxon::Max(0.0, rgb.z));
	const maxon::Float64 largest = maxon::Max(rgb.x, maxon::Max(rgb.y, rgb.z));
	return largest > 0.0 ? rgb / largest : cinema::Vector(1.0);
}

// Whether the Redshift light has a texture for its colour: a parameter of Redshift's own file type
// (REDSHIFT_FILE_TYPE), read as the dome light's (environment_export.cpp).
static maxon::Bool HasRedshiftTexture(const cinema::BaseObject* light)
{
	cinema::GeData data;
	if (!light->GetParameter(CreateDescID(cinema::DescLevel(REDSHIFT_LIGHT_PHYSICAL_TEXTURE, REDSHIFT_FILE_TYPE, 0), cinema::DescLevel(REDSHIFT_FILE_PATH, cinema::DTYPE_STRING, 0)),
		data, cinema::DESCFLAGS_GET::NONE))
		return false;
	return data.GetType() == cinema::DA_STRING && !data.GetString().IsEmpty();
}

// Whether this Redshift counts its units as Redshift 2026 and 2027 do, the versions measured (their
// lights list the units 5 and 6). Redshift 3.6.04, Cinema 4D 2024's, does not: there a point or spot
// light in "Image" and a normalized area light depend on Redshift's unit scale (1 cm, the default of
// a new document from Python: about 10000 times darker than 2027's; 1 m: the same), and the photometric
// units on its photometric settings (lm: 10000 where 2027 has 683, at their defaults). Asked once:
// Redshift does not change in a session.
static maxon::Bool RedshiftUnitsAsMeasured(const cinema::BaseObject* light)
{
	static maxon::AtomicInt32 known;   // 0 not yet asked, 1 yes, 2 no
	const maxon::Int32 state = known.Load();
	if (state != 0)
		return state == 1;
	maxon::Bool current = false;
	cinema::AutoAlloc<cinema::Description> description;
	if (description && light->GetDescription(description, cinema::DESCFLAGS_DESC::NONE))
	{
		const cinema::BaseContainer* const parameter = description->GetParameterI(ConstDescIDLevel(REDSHIFT_LIGHT_PHYSICAL_UNITSTYPE), nullptr);
		const cinema::BaseContainer* const cycle = parameter ? parameter->GetContainerInstance(cinema::DESC_CYCLE) : nullptr;
		current = cycle && cycle->GetDataPointer(REDSHIFT_UNITS_LUMINOUS_INTENSITY) != nullptr;
	}
	known.Set(current ? 1 : 2);
	return current;
}

// Redshift's render setting "Image Units Scale" (REDSHIFT_RENDERER_IMAGE_UNITS_SCALE, from
// Redshift 2026 on; 100 by default), read from a Redshift video post of the document's active render
// data (a scene set up for Redshift keeps it when Cycles Render is chosen), else its default. A point
// or spot light in "Image" is (scale / 100)^2 times as strong (measured: 50 gives 0.25, 200 gives 4.0;
// the other units, area and infinite lights do not change).
static maxon::Float64 RedshiftImageUnitsScale(const cinema::BaseObject* light)
{
	const cinema::BaseDocument* const doc = light->GetDocument();
	const cinema::RenderData* const renderData = doc ? doc->GetActiveRenderData() : nullptr;
	for (const cinema::BaseVideoPost* post = renderData ? FirstVideoPost(renderData) : nullptr; post; post = post->GetNext())
	{
		if (post->GetType() != REDSHIFT_RENDERER)
			continue;
		const maxon::Float64 scale = GetFloatParameter(post, REDSHIFT_RENDERER_IMAGE_UNITS_SCALE, 100.0);
		return scale > 0.0 ? scale : 100.0;
	}
	return 100.0;
}

// Redshift's area light "Spread" s (1: a plain diffuse emitter) in Blender's terms. Measured with
// Redshift 2027.0.0 (a 10 cm light 1 m over a floor): within the half angle s * 90 degrees the light
// is the diffuse one times 1 / (1 - cos(s * 90)), beyond it nothing (s 0.8: 1.447 to 72 degrees;
// s 0.5: 3.414 to 45; s 0.3: 9.17 to 27). Cycles' spread is another profile: from the axis it falls
// with the tangent of the angle to nothing at half the spread, its power kept (Blender's
// intern/cycles/kernel/light/area.h, read; the model reproduces Cycles' measured floor to 2 %).
// The table gives, per s, Blender's half angle and a factor for the power that come closest to
// Redshift's light on a floor below a small light (least squares of the floor's brightness, weighted
// by the floor's area, out to 80 degrees; docs/reference/c4d-sdk.md, "Redshift lights"). What is left
// is the difference of the profiles: Blender's edge is soft and lies farther out.
// Sets `spread` (Blender's full angle) and returns the factor for the power.
static maxon::Float64 RedshiftSpread(maxon::Float64 s, maxon::Float64& spread)
{
	// s in steps of 0.05 from 0 to 1: Blender's half angle in degrees, the power factor.
	static const maxon::Float64 TABLE[21][2] = {
		{ 0.0, 2.000 }, { 6.00, 1.988 }, { 12.05, 1.984 }, { 18.05, 1.979 }, { 24.10, 1.961 }, { 30.25, 1.949 }, { 36.40, 1.926 },
		{ 42.70, 1.908 }, { 49.00, 1.883 }, { 55.35, 1.857 }, { 61.85, 1.836 }, { 68.25, 1.809 }, { 74.40, 1.775 }, { 79.85, 1.721 },
		{ 84.25, 1.644 }, { 87.35, 1.543 }, { 89.05, 1.423 }, { 89.80, 1.300 }, { 90.0, 1.184 }, { 90.0, 1.084 }, { 90.0, 1.000 } };
	spread = maxon::PI;
	const maxon::Float64 position = maxon::ClampValue(s, 0.0, 1.0) * 20.0;
	const maxon::Int index = maxon::Min(maxon::Int(position), maxon::Int(19));
	const maxon::Float64 t = position - maxon::Float64(index);
	const maxon::Float64 halfAngle = (TABLE[index][0] + t * (TABLE[index + 1][0] - TABLE[index][0])) * maxon::PI / 180.0;
	const maxon::Float64 factor = TABLE[index][1] + t * (TABLE[index + 1][1] - TABLE[index][1]);
	if (halfAngle < maxon::PI * 0.5 - 1e-6)
		spread = 2.0 * halfAngle;
	return factor;
}

static maxon::Bool TranslateRedshiftLight(const cinema::BaseObject* light, const Coords& coords, const DocumentColors& colors, maxon::Float64 multiplier,
	const cinema::Vector& scale, BlenderLight& out, SceneWriter* writer)
{
	const maxon::Float64 value = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_INTENSITY, 1.0) *
		maxon::Pow(2.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_EXPOSURE, 0.0)) * multiplier;
	if (value <= 0.0)
		return false;

	const maxon::Int32 type = GetIntParameter(light, REDSHIFT_LIGHT_TYPE, REDSHIFT_LIGHT_TYPE_PHYSICAL_POINT);
	if (type == REDSHIFT_LIGHT_TYPE_DOME)
		return false;   // the environment: scene_exporter.cpp hands it to ExportWorldItem
	if (type == REDSHIFT_LIGHT_TYPE_PORTAL)
	{
		Warn(writer, FormatString("Light \"@\": Redshift portal lights are not translated; Cycles samples the environment without them.", light->GetName()));
		return false;
	}

	// The colour: the light's colour, its temperature's, or both multiplied (measured).
	const cinema::Vector color = GetVectorParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLOR, cinema::Vector(1.0));
	const maxon::Int32 colorMode = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_COLORMODE, REDSHIFT_LIGHT_COLORMODE_COLOR);
	if (colorMode == REDSHIFT_LIGHT_COLORMODE_COLOR)
	{
		out.color = color;
	}
	else
	{
		const cinema::Vector warm = RedshiftTemperatureColor(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_TEMPERATURE, 6500.0), colors);
		out.color = colorMode == REDSHIFT_LIGHT_COLORMODE_TEMPERATURE_AND_COLOR ? cinema::Vector(warm.x * color.x, warm.y * color.y, warm.z * color.z) : warm;
	}
	if (HasRedshiftTexture(light))
		Warn(writer, FormatString("Light \"@\": a texture on a Redshift light is not translated; the light's colour is used.", light->GetName()));
	out.useShadow = RedshiftLightCastsShadow(light);

	const maxon::Int32 unit = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_UNITSTYPE, REDSHIFT_LIGHT_UNITSTYPE_IMAGE);
	const maxon::Float64 efficacy = maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_LUMENSPERWATT, 17.0));
	// A point or spot light in "Image" follows Redshift's render setting "Image Units Scale".
	const maxon::Bool pointLike = type != REDSHIFT_LIGHT_TYPE_PHYSICAL_INFINITE && type != REDSHIFT_LIGHT_TYPE_PHYSICALSUN && type != REDSHIFT_LIGHT_TYPE_PHYSICAL_AREA;
	maxon::Float64 imageScale = 1.0;
	if (pointLike && unit == REDSHIFT_LIGHT_UNITSTYPE_IMAGE)
	{
		const maxon::Float64 ratio = RedshiftImageUnitsScale(light) / 100.0;
		imageScale = ratio * ratio;
	}
	// An older Redshift counts these otherwise; named, translated as the measured versions render them.
	// Measured alike in Redshift 3.6.04: area and infinite lights in "Image" with their shapes, scale and
	// colour temperature, without "Normalize Intensity", a spread or a decay other than the inverse square.
	const maxon::Bool area = type == REDSHIFT_LIGHT_TYPE_PHYSICAL_AREA;
	const maxon::Bool alike = !pointLike && unit == REDSHIFT_LIGHT_UNITSTYPE_IMAGE &&
		!(area && GetBoolParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_NORMALIZE, false)) &&
		!(area && GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SPREAD, 1.0) < 0.9999) &&
		(!area || GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_DECAYTYPE, REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE) == REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE);
	if (!alike && !RedshiftUnitsAsMeasured(light))
		Warn(writer, FormatString("Light \"@\": this Redshift is older than 2026 and renders point and spot lights, other units than \"Image\", \"Normalize Intensity\", spread and decay otherwise; the light is translated as Redshift 2026 and 2027 render it.", light->GetName()));
	const maxon::Float64 scaleX = maxon::Abs(scale.x);
	const maxon::Float64 scaleY = maxon::Abs(scale.y);
	const maxon::Float64 scaleZ = maxon::Abs(scale.z);
	switch (type)
	{
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_INFINITE:
		case REDSHIFT_LIGHT_TYPE_PHYSICALSUN:
		{
			if (type == REDSHIFT_LIGHT_TYPE_PHYSICALSUN)
				Warn(writer, FormatString("Light \"@\": the Redshift sun is rendered as a plain infinite light.", light->GetName()));
			out.type = BLENDERLIGHT::SUN;
			out.energy = value * RedshiftUnitFactor(unit, efficacy, 4.0 * maxon::PI, 0.0, true);
			out.angle = 0.0;
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_SPOT:
		{
			const maxon::Float64 cone = maxon::ClampValue(GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_ANGLE, maxon::PI * 40.0 / 180.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 falloff = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_FALLOFF_ANGLE, 0.0);
			// Lumens and watts are spread over the cone, its falloff included (measured).
			const maxon::Float64 coneSolidAngle = 2.0 * maxon::PI * (1.0 - maxon::Cos(cone * 0.5));
			out.type = BLENDERLIGHT::SPOT;
			out.energy = 4.0 * maxon::PI * value * RedshiftUnitFactor(unit, efficacy, coneSolidAngle, 0.0, false) * imageScale;
			out.radius = 0.0;
			out.spotSize = cone;
			out.spotBlend = SpotBlend(maxon::Max(0.0, cone - 2.0 * falloff), cone);
			if (GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_SPOT_CONE_FALLOFF_CURVE, 0.0) > 0.0)
				Warn(writer, FormatString("Light \"@\": the spot's \"Falloff Curve\" is not translated; Blender's spot is evenly bright inside its cone.", light->GetName()));
			break;
		}
		case REDSHIFT_LIGHT_TYPE_PHYSICAL_AREA:
		{
			const maxon::Int32 geometry = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_GEOMETRY, REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE);
			const maxon::Float64 sizeX = coords.Length(maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEX, 200.0))) * scaleX;
			const maxon::Float64 sizeY = coords.Length(maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEY, 200.0))) * scaleY;
			const maxon::Float64 sizeZ = coords.Length(maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SIZEZ, 200.0))) * scaleZ;
			const maxon::Bool normalized = GetBoolParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_NORMALIZE, false);
			// The surface that emits, as Redshift counts it for "Normalize" and the units (measured):
			// a sphere's diameter is Size X (Size Y and Z only change how it looks), a cylinder of the
			// diameters Size X and Size Y runs along its Z by Size Z (its side emits).
			maxon::Float64 surface = sizeX * sizeY;
			maxon::Float64 diameter = 0.0;
			switch (geometry)
			{
				case REDSHIFT_LIGHT_AREA_GEOMETRY_DISC:
					surface = maxon::PI * 0.25 * sizeX * sizeY;
					break;
				case REDSHIFT_LIGHT_AREA_GEOMETRY_SPHERE:
					diameter = sizeX;
					surface = maxon::PI * diameter * diameter;
					break;
				case REDSHIFT_LIGHT_AREA_GEOMETRY_CYLINDER:
					diameter = 0.5 * (sizeX + sizeY);
					surface = maxon::PI * diameter * sizeZ;
					break;
				case REDSHIFT_LIGHT_AREA_GEOMETRY_RECTANGLE:
					break;
				default:
					Warn(writer, FormatString("Light \"@\": a mesh as the shape of a Redshift area light is not translated; it is rendered as a rectangle of its size.", light->GetName()));
					break;
			}
			if (surface <= 0.0)
				return false;
			// Its radiance in Redshift's terms of "Image": "Normalize" counts in that unit only.
			const maxon::Float64 radiance = value * RedshiftUnitFactor(unit, efficacy, 0.0, surface, false) /
				((unit == REDSHIFT_LIGHT_UNITSTYPE_IMAGE && normalized) ? surface : 1.0);
			if (geometry == REDSHIFT_LIGHT_AREA_GEOMETRY_SPHERE)
			{
				// Blender's point light with a radius is that sphere: power pi * L * (pi d^2).
				// Blender's "Soft Falloff" (on by default) lights as a disc facing the point would, 1 / (1 + (r/d)^2)
				// of the sphere (measured: 0.941 of Redshift's at r/d = 0.25); off, it is the sphere.
				out.type = BLENDERLIGHT::POINT;
				out.radius = 0.5 * diameter;
				out.softFalloff = false;
				out.energy = maxon::PI * radiance * surface;
				break;
			}
			if (geometry == REDSHIFT_LIGHT_AREA_GEOMETRY_CYLINDER)
			{
				// Blender has no tube. A sphere as bright as the tube seen from its side: Redshift's tube
				// lights a floor below its middle as a diffuse tube of twice its radiance would (measured
				// for round tubes, 2.0 within 4 %), radiant intensity 2 L d l there, so power 4 pi of that.
				Warn(writer, FormatString("Light \"@\": Blender has no cylinder light; it is rendered as a sphere as bright as the cylinder seen from its side (brighter along its axis).", light->GetName()));
				out.type = BLENDERLIGHT::POINT;
				out.radius = 0.5 * diameter;
				out.softFalloff = false;
				out.energy = 4.0 * maxon::PI * 2.0 * radiance * diameter * sizeZ;
				break;
			}
			out.type = BLENDERLIGHT::AREA;
			const maxon::Bool round = geometry == REDSHIFT_LIGHT_AREA_GEOMETRY_DISC;
			const maxon::Bool square = maxon::Abs(sizeX - sizeY) <= 1e-9 * maxon::Max(sizeX, sizeY);
			out.shape = round ? (square ? BLENDERAREA::DISK : BLENDERAREA::ELLIPSE) : BLENDERAREA::RECTANGLE;
			out.size = sizeX;
			out.sizeY = sizeY;
			const maxon::Float64 spread = GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_SPREAD, 1.0);
			out.energy = maxon::PI * radiance * surface * RedshiftSpread(spread, out.spread);
			if (spread < 0.9999)
				Warn(writer, FormatString("Light \"@\": Blender's spread is shaped otherwise than Redshift's: matched in strength, its edge is softer and lies farther out (Eevee ignores the spread).", light->GetName()));
			// Seen by the camera as in Redshift (its ray contribution "Camera", on by default).
			out.rayVisibility = true;
			out.visibleCamera = GetFloatParameter(light, REDSHIFT_CAMERA_CONTRIBUTION, 1.0) > 0.0;
			out.twoSided = GetBoolParameter(light, REDSHIFT_LIGHT_PHYSICAL_AREA_BIDIRECTIONAL, false);
			break;
		}
		default:
		{
			if (type != REDSHIFT_LIGHT_TYPE_PHYSICAL_POINT)
				Warn(writer, FormatString("Light \"@\": this Redshift light type is rendered as a point light.", light->GetName()));
			out.type = BLENDERLIGHT::POINT;
			out.energy = 4.0 * maxon::PI * value * RedshiftUnitFactor(unit, efficacy, 4.0 * maxon::PI, 0.0, false) * imageScale;
			out.radius = 0.0;
			break;
		}
	}
	if (out.energy <= 0.0)
	{
		Warn(writer, FormatString("Light \"@\": Redshift renders this light black in its unit (an area light's unit on a point or spot light); it is left out.", light->GetName()));
		return false;
	}

	// Decay. Blender's lights always fall off with the square of the distance. Redshift's "None" is
	// as bright at any distance as its inverse square is at 1 m, and "Linear" falls from that to
	// nothing between Start and Stop (measured; Start and Stop count for "Linear" only). Matched:
	// "None" at 1 m (the light is unchanged), "Linear" halfway between Start and Stop.
	if (type != REDSHIFT_LIGHT_TYPE_PHYSICAL_INFINITE && type != REDSHIFT_LIGHT_TYPE_PHYSICALSUN)
	{
		const maxon::Int32 decay = GetIntParameter(light, REDSHIFT_LIGHT_PHYSICAL_DECAYTYPE, REDSHIFT_LIGHT_DECAYTYPE_INVERSE_SQUARE);
		if (decay == REDSHIFT_LIGHT_DECAYTYPE_NONE)
		{
			Warn(writer, FormatString("Light \"@\": Blender's lights fall off with the square of the distance; Redshift's decay \"None\" is matched 1 m from the light (brighter closer, darker farther).", light->GetName()));
		}
		else if (decay == REDSHIFT_LIGHT_DECAYTYPE_LINEAR)
		{
			const maxon::Float64 start = coords.Length(maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_FALLOFFSTART, 0.0)));
			const maxon::Float64 stop = coords.Length(maxon::Max(0.0, GetFloatParameter(light, REDSHIFT_LIGHT_PHYSICAL_FALLOFFSTOP, 100.0)));
			// Halfway the ramp is at one half; a ramp without length is a step at Start.
			const maxon::Float64 middle = stop > start ? 0.5 * (start + stop) : start;
			const maxon::Float64 ramp = stop > start ? 0.5 : 1.0;
			out.energy *= ramp * middle * middle;   // metres: 1 m is where Redshift's light is v
			Warn(writer, FormatString("Light \"@\": Blender's lights fall off with the square of the distance; Redshift's linear decay is matched halfway between its Start and Stop.", light->GetName()));
			if (out.energy <= 0.0)
				return false;
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
	// Every standard light casts shadows, also with "Shadow" set to None (Cinema 4D's default, under
	// which the Standard renderer casts none): the maintainer's decision. A Cycles
	// Light tag's Settings tab turns them off (ExportLight). The shadow type decides how soft they are
	// (ApplyShadowSoftness): None and Raytraced (Hard) sharp, Area and Shadow Maps (Soft) through the
	// light's size; an area light keeps its own size whatever the type.
	out.useShadow = true;

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
			ApplyShadowSoftness(light, coords, reference, out, writer);
			break;
		case BLENDERLIGHT::POINT:
			out.energy = brightness * POINT_POWER * reference * reference;
			out.radius = sphere ? sizeX * 0.5 : 0.0;
			// A sphere-shaped area light has a size of its own, which wins over the shadow type.
			if (!sphere)
				ApplyShadowSoftness(light, coords, reference, out, writer);
			break;
		case BLENDERLIGHT::SPOT:
		{
			const maxon::Float64 outer = maxon::ClampValue(GetFloatParameter(light, LIGHT_DETAILS_OUTERANGLE, maxon::PI / 6.0), MIN_SPOT_SIZE, MAX_SPOT_SIZE);
			const maxon::Float64 inner = GetFloatParameter(light, LIGHT_DETAILS_INNERANGLE, 0.0);
			out.energy = brightness * POINT_POWER * reference * reference;
			out.radius = 0.0;
			out.spotSize = outer;
			out.spotBlend = SpotBlend(inner, outer);
			ApplyShadowSoftness(light, coords, reference, out, writer);
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
	const cinema::Matrix& mg, BlenderLight& out, SceneWriter* writer)
{
	if (!light)
		return false;
	if (light->GetType() == Orslight)
	{
		const cinema::Vector scale(mg.sqmat.v1.GetLength(), mg.sqmat.v2.GetLength(), mg.sqmat.v3.GetLength());
		return TranslateRedshiftLight(light, coords, colors, multiplier, scale, out, writer);
	}
	return TranslateStandardLight(light, coords, colors, multiplier, out, writer);
}

maxon::String BackLightId(const maxon::String& id)
{
	return id + "_back"_s;
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
// field for field, and a Redshift area light's spread, a sphere's soft falloff and the camera's
// view when it has them; the extra blocks only when they were filled.
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
			// A translated sphere (a Redshift area light's) is lit as a sphere: Soft Falloff off.
			if (!light.blenderSettings && !light.softFalloff)
				json.Bool("use_soft_falloff", false);
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
			// A translated light's spread (a Redshift area light's) only when it is not Blender's default.
			if (!light.blenderSettings && light.spread < maxon::PI)
				json.Float("spread", light.spread);
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

// Eevee of Blender 5.0 and 5.1 shows no light to the camera, whatever the light's visibility.
static void NoteCameraVisibility(SceneWriter& writer, const cinema::BaseObject* light, const BlenderLight& values, maxon::Bool eeveeHidesLights)
{
	if (eeveeHidesLights && values.rayVisibility && values.visibleCamera)
	{
		writer.Warn(FormatString("Light \"@\": its Camera visibility needs Blender 5.2 or newer with Eevee; this Eevee does not show the light itself.",
			light->GetName()));
	}
}

// ExportLight without the bookkeeping of the other side: sets `written` when a two-sided light wrote
// its other side (BackLightId).
static maxon::Bool WriteTranslatedLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking, maxon::Bool eeveeHidesLights,
	maxon::Bool& written)
{
	// The green check in the Object Manager. The walk leaves such a light out already; this keeps a
	// light written alone (the IPR during a drag, SceneExporter::WriteLightAlone) in step with it.
	if (!IsLightSwitchedOn(light))
		return false;

	BlenderLight values;
	if (light->GetType() == ID_LIGHT_OBJECT)
	{
		// The plugin's own light: Blender's values, every tab.
		ReadLightPanel(light, coords, colors, values);
		ReadCyclesSettings(light, values);
		ReadRayVisibility(light, values);
		ReadEeveeSettings(light, coords, values);
		values.energy *= multiplier;
		NoteCameraVisibility(writer, light, values, eeveeHidesLights);
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
		// Shadows as without the Light tab (TranslateLight: always for a standard light, the light's
		// own switch for a Redshift light) until the Settings tab says otherwise.
		const maxon::Bool shadow = light->GetType() != Orslight || RedshiftLightCastsShadow(light);
		ReadLightPanel(tag, coords, colors, values);
		values.useShadow = shadow;
		values.energy *= multiplier;
		// How the light is seen stays the light's own (a Redshift area light's camera visibility),
		// until the Visibility tab says otherwise; what it emits, its back side included, is the tab's.
		BlenderLight own;
		if (light->GetType() == Orslight && TranslateLight(light, coords, colors, multiplier, mg, own, nullptr) && own.rayVisibility)
		{
			values.rayVisibility = true;
			values.visibleCamera = own.visibleCamera;
		}
	}
	else if (!TranslateLight(light, coords, colors, multiplier, mg, values, &writer))
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
	NoteCameraVisibility(writer, light, values, eeveeHidesLights);
	WriteLight(writer, coords, mg, id, values, linking);
	if (values.twoSided && values.type == BLENDERLIGHT::AREA)
	{
		// The other side: the same light turned half a turn about its Y axis.
		cinema::Matrix back = mg;
		back.sqmat.v1 = -back.sqmat.v1;
		back.sqmat.v3 = -back.sqmat.v3;
		WriteLight(writer, coords, back, BackLightId(id), values, linking);
		written = true;
	}
	return true;
}

maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking, maxon::Bool eeveeHidesLights)
{
	maxon::Bool back = false;
	const maxon::Bool written = WriteTranslatedLight(writer, coords, colors, light, mg, id, multiplier, linking, eeveeHidesLights, back);
	// A light written alone (the IPR during a drag) that no longer shines to both sides, or not at
	// all, takes its other side out of Blender's scene; a whole export leaves it out by itself.
	if (!back)
		writer.RemoveItem(BackLightId(id));
	return written;
}

} // namespace cyclesrender
