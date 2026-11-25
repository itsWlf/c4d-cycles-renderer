// SPDX-License-Identifier: Apache-2.0
// What a Redshift camera does to the picture beyond what a camera is (its exposure, 0.3.0, F13 W6), read
// so that Blender's picture gets the same.

#ifndef CYCLESRENDER_REDSHIFT_CAMERA_H__
#define CYCLESRENDER_REDSHIFT_CAMERA_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The exposure of a Redshift camera (Orscamera) as Redshift applies it: one factor on the scene-linear
/// picture before the view transform. Measured against Redshift 3.6.04 (Cinema 4D 2024.5), 2026.3 and
/// 2027.0 (docs/reference/c4d-sdk.md, "The Redshift camera's exposure"):
///
///   "EV Only"   k = 2^EV
///   "Filmic"    k = C * 2^EV * (ISO / 100) * 60 t * (8 / N)^2
///               t the exposure time: 1 / Shutter Time with the shutter type "Still", the Shutter Angle as
///               a share of a turn over the render settings' frame rate with "Movie"; N the Aperture (f/#);
///               C = 2.8715, or 683/10000 of it under the Units Mode "Project Settings" of Redshift 2026 on
///               (the default of a new document there).
///
/// A Cinema 4D camera is exposed by nothing of Redshift's (the photographic exposure groups of the RS Camera
/// tag and of the RS Post-Effects video post change nothing in Redshift 2027.0, measured).
//----------------------------------------------------------------------------------------
struct RedshiftExposure
{
	maxon::Float64 stops = 0.0;								///< log2 of the factor; 0 for every camera but a Redshift camera
	maxon::BaseArray<maxon::String> notes;		///< what of the camera's picture settings is not translated: a console line each
};

/// Reads the exposure of @p camera (any camera; only a Redshift camera has one).
maxon::Result<RedshiftExposure> ReadRedshiftExposure(const cinema::BaseObject* camera);

struct ImagerChain;

/// Which of Redshift's effects on the picture are translated: the Render Settings' "Scene Translation" > "Redshift",
/// "Camera and Post Effects" (all of them) and, below it, one switch per effect; each on by default.
struct RedshiftTranslation
{
	maxon::Bool exposure = true;
	maxon::Bool whitePoint = true;
	maxon::Bool vignetting = true;
	maxon::Bool toneMapping = true;			///< with the saturation it carries
	maxon::Bool bloom = true;
	maxon::Bool streak = true;
	maxon::Bool flare = true;
	maxon::Bool colorControls = true;		///< their exposure, and the lines naming their contrast and curves
	maxon::Bool all = true;							///< the single switch: off, nothing is translated or named (the LUT, which has no switch of its own, too)

	/// Nothing translated (the single switch off).
	static RedshiftTranslation None()
	{
		RedshiftTranslation none;
		none.exposure = none.whitePoint = none.vignetting = none.toneMapping = none.bloom = none.streak = none.flare = none.colorControls = false;
		none.all = false;
		return none;
	}
};

//----------------------------------------------------------------------------------------
/// What Redshift does to the picture of @p camera after its exposure, appended to @p chain as layers of the
/// Imager's effects (0.3.0, F13 W7 and W8), in the order Redshift applies them (measured, docs/reference/c4d-sdk.md,
/// "The Redshift camera's effects"): White Balance (the camera's White Point), Lens Vignetting (its Vignetting),
/// Lens Glare for bloom, streak and flare, Tone Mapping (its Tone-Mapping and Saturation), then the colour
/// controls' exposure. Tone mapping, white point, saturation and vignetting are a Redshift camera's own; bloom, streak,
/// flare, LUT and colour controls come from the RS Post-Effects video post, or from the Redshift camera where its group
/// says "Override" (none where it says "Off"); a Cinema 4D camera takes the video post's. What is not translated (the
/// LUT, the colour controls' contrast and curves, tints, Blacks, a streak's tail) is named in @p chain's notes.
/// An effect @p which leaves out is neither translated nor named.
//----------------------------------------------------------------------------------------
maxon::Result<void> ReadRedshiftEffects(const cinema::BaseObject* camera, ImagerChain& chain, const RedshiftTranslation& which);

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_CAMERA_H__
