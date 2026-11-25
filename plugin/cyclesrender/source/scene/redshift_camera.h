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

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_CAMERA_H__
