// SPDX-License-Identifier: Apache-2.0
// The "Cycles Camera" tag: Blender's depth of field and lens settings, on a camera.

#ifndef CYCLESRENDER_CAMERA_TAG_H__
#define CYCLESRENDER_CAMERA_TAG_H__

#include "c4d.h"

namespace cyclesrender
{

/// Blender's defaults for a new camera (DNA_camera_types.h of Blender 5.2.3), which a new tag
/// starts with and the exporter falls back on. Lengths in document units (10 m and 6 m in a
/// document in centimetres), angles in radians.
namespace CAMERATAG_DEFAULT
{
	static const maxon::Float64 FOCUS_DISTANCE = 1000.0;
	static const maxon::Float64 FSTOP = 2.8;
	static const maxon::Int32 BLADES = 0;
	static const maxon::Float64 ROTATION = 0.0;
	static const maxon::Float64 RATIO = 1.0;
	static const maxon::Float64 ORTHO_SCALE = 600.0;
	static const maxon::Float64 FISHEYE_FOV = 3.14159265358979323846;
	static const maxon::Float64 FISHEYE_LENS = 10.5;
	static const maxon::Float64 LATITUDE = 1.57079632679489661923;     ///< max; min is its negative
	static const maxon::Float64 LONGITUDE = 3.14159265358979323846;    ///< max; min is its negative
	static const maxon::Float64 POLYNOMIAL[5] = { -1.1735143712967577e-05, -0.019988736953434998, -3.3525322965709175e-06,
		3.099275275886036e-06, -2.6064646454854524e-08 };
	static const maxon::Float64 CYLINDER_U = 3.14159265358979323846;   ///< max; min is its negative
	static const maxon::Float64 CYLINDER_V = 1.0;                      ///< max; min is its negative
	static const maxon::Float64 CYLINDER_RADIUS = 1.0;
}

/// Registers the tag with Cinema 4D. Called once from PluginStart(). The exporter reads the
/// tag's parameters straight from its container (ids in res/description/tcyclescamera.h), with
/// the same defaults as Init() sets here (scene/camera_export.cpp, CameraValues::Read).
cinema::Bool RegisterCameraTag();

/// The Cycles Camera tag that counts for @p camera: the first one on the camera itself. A tag
/// on a parent counts for nothing (unlike the Cycles Object tag), because a camera's settings
/// belong to that camera alone. Null when the camera has none.
const cinema::BaseTag* FindCameraTag(const cinema::BaseObject* camera);

} // namespace cyclesrender

#endif // CYCLESRENDER_CAMERA_TAG_H__
