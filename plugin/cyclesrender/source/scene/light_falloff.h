// SPDX-License-Identifier: Apache-2.0
// The falloff and the photometric intensity of Cinema 4D's standard lights (0.3.0, F13 W29): what the
// Standard renderer does with them, as a light node tree for Cycles and an approximation for Eevee.

#ifndef CYCLESRENDER_LIGHT_FALLOFF_H__
#define CYCLESRENDER_LIGHT_FALLOFF_H__

#include "c4d.h"

#include "../session/json_writer.h"
#include "coords.h"
#include "light_export.h"
#include "scene_writer.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The factor a standard light's "Photometric Intensity" puts on its brightness, 1 without it
/// (measured with the Standard renderer, docs/reference/c4d-sdk.md, "Light conversion"): the
/// brightness times the intensity in candela over 1000, a lumen value spread over the whole sphere,
/// or over a spot's cone (its outer angle). Lux (which Cinema 4D 2027 does not offer these lights)
/// is named in a warning to @p writer and left out. (An IES profile belongs to the light type
/// "Photometric", which is named as not supported.)
/// @param[out] photometric  The intensity counts: an inverse square then falls off from 1 m.
//----------------------------------------------------------------------------------------
maxon::Float64 PhotometricFactor(const cinema::BaseObject* light, maxon::Bool& photometric, SceneWriter* writer);

//----------------------------------------------------------------------------------------
/// Reads the falloff of the standard light @p light into @p out (BlenderLight::falloff and the
/// fields after it) and returns the distance in metres its energy is to be matched at: the
/// Radius/Decay, or 1 m for an inverse square with photometric intensity. An infinite light (@p sun)
/// keeps Blender's sun: its falloff, which the Standard renderer has, is named in a warning.
/// @param[in] brightness  The light's brightness with its photometric factor, without the Light Multiplier.
/// @param[in] color       The light's colour in the render space (the inverse square's 0.01 counts against its brightest channel).
//----------------------------------------------------------------------------------------
maxon::Float64 ReadStandardFalloff(const cinema::BaseObject* light, const Coords& coords, maxon::Bool sun, maxon::Bool photometric,
	maxon::Float64 brightness, const cinema::Vector& color, BlenderLight& out, SceneWriter* writer);

//----------------------------------------------------------------------------------------
/// Eevee evaluates no light node trees: turns the falloff of @p values into a power and a custom
/// distance (BlenderLight::eeveeCutoff) and clears it. Step and Linear are matched halfway along
/// their ramp and end at the radius (Eevee's custom distance fades a light as (1 - (d/c)^4)^2,
/// measured, which the power makes up for there); None and Inverse Square Clamped stay matched at
/// the radius; the inverse square is left to Eevee's own Light Threshold. Every approximation is
/// named in a warning.
//----------------------------------------------------------------------------------------
void FalloffForEevee(const cinema::BaseObject* light, BlenderLight& values, SceneWriter& writer);

//----------------------------------------------------------------------------------------
/// Writes the members "nodes" and "links" of the light item open in @p json: the light node tree
/// that gives Cycles the falloff of @p light (nothing for PHYSICAL). The tree multiplies the
/// light, which its energy makes B (r/d)^2, by (d/r)^2 times the falloff's own shape, less the
/// inverse square's 0.01, with d the Light Path node's Ray Length.
//----------------------------------------------------------------------------------------
void WriteFalloffTree(JsonWriter& json, const BlenderLight& light);

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_FALLOFF_H__
