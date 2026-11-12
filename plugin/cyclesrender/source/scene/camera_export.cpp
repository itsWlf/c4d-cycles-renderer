// SPDX-License-Identifier: Apache-2.0
// The camera as a `camera` item, or as a free view of Blender's viewport (camera_export.h).
//
// Reading order: CameraValues::Read (Cinema 4D's projections, its spherical camera, then the
// Cycles Camera tag on top), then ExportCamera. The measurements behind the projection rules
// are in docs/reference/c4d-sdk.md, "Cameras and views".

#include "c4d.h"
#include "ocamera.h"
#include "tcyclescamera.h"

#include "../tags/camera_tag.h"
#include "camera_export.h"
#include "parameters.h"

namespace cyclesrender
{

// Without a clipping plane set on the camera, these limits (in metres) are used. Cycles does
// not care; Eevee's depth buffer loses precision when the range is much wider.
static const maxon::Float64 DEFAULT_CLIP_START = 0.01;
static const maxon::Float64 DEFAULT_CLIP_END = 10000.0;
// Blender's smallest clip_start.
static const maxon::Float64 MIN_CLIP_START = 1e-6;

// Where Cinema 4D clips nothing (its fixed views, a parallel camera without near clipping),
// not even behind the camera, Blender's orthographic camera is moved back this far along its
// view (metres) and sees as far again beyond the camera's plane.
static const maxon::Float64 UNCLIPPED_DEPTH = 10000.0;

// In parallel projection Cinema 4D shows 1024 document units across the image at zoom 1,
// whatever the resolution.
static const maxon::Float64 PARALLEL_WIDTH_AT_ZOOM_1 = 1024.0;

// Cinema 4D's spherical camera (the Spherical tab of a camera; ocamera.h).
static const maxon::Int32 SPHERICAL_MAPPING_LATLONG = 0;
static const maxon::Int32 SPHERICAL_GIZMO_DOME = 1;

//----------------------------------------------------------------------------------------
// Cinema 4D's fixed views (CAMERA_PROJECTION Left to Dimetric). The camera's own rotation does
// not count for them, only its position (measured: a rotated camera renders the same picture).
// The axes are those BaseView::GetMg() gives for each projection of a view; a slant (oblique)
// is BaseView::GetViewParameter()'s scale_z, which moves a point at depth z by
// (scale_z.x * z, -scale_z.y * z) across the picture.
//----------------------------------------------------------------------------------------
struct FixedView
{
	maxon::Int32 projection;
	cinema::Vector v1;
	cinema::Vector v2;
	cinema::Vector v3;
	maxon::Float64 slantX;
	maxon::Float64 slantY;
};

static const maxon::Float64 SQRT3_2 = 0.86602540378443865;
static const maxon::Float64 SQRT1_2 = 0.70710678118654752;
static const maxon::Float64 SQRT1_6 = 0.40824829046386302;
static const maxon::Float64 SQRT1_3 = 0.57735026918962576;

static const FixedView FIXED_VIEWS[] =
{
	{ Pleft, cinema::Vector(0, 0, -1), cinema::Vector(0, 1, 0), cinema::Vector(1, 0, 0), 0.0, 0.0 },
	{ Pright, cinema::Vector(0, 0, 1), cinema::Vector(0, 1, 0), cinema::Vector(-1, 0, 0), 0.0, 0.0 },
	{ Pfront, cinema::Vector(1, 0, 0), cinema::Vector(0, 1, 0), cinema::Vector(0, 0, 1), 0.0, 0.0 },
	{ Pback, cinema::Vector(-1, 0, 0), cinema::Vector(0, 1, 0), cinema::Vector(0, 0, -1), 0.0, 0.0 },
	{ Ptop, cinema::Vector(1, 0, 0), cinema::Vector(0, 0, 1), cinema::Vector(0, -1, 0), 0.0, 0.0 },
	{ Pbottom, cinema::Vector(1, 0, 0), cinema::Vector(0, 0, -1), cinema::Vector(0, 1, 0), 0.0, 0.0 },
	{ Pmilitary, cinema::Vector(-SQRT3_2, 0, 0.5), cinema::Vector(-0.5, 0, -SQRT3_2), cinema::Vector(0, -1, 0), 0.0, 1.0 },
	{ Pfrog, cinema::Vector(-SQRT3_2, 0, 0.5), cinema::Vector(-0.5, 0, -SQRT3_2), cinema::Vector(0, -1, 0), 0.0, 2.0 },
	{ Pbird, cinema::Vector(-SQRT3_2, 0, 0.5), cinema::Vector(-0.5, 0, -SQRT3_2), cinema::Vector(0, -1, 0), 0.0, 0.5 },
	{ Pgentleman, cinema::Vector(1, 0, 0), cinema::Vector(0, 1, 0), cinema::Vector(0, 0, 1), 0.3535533905932738, -0.3535533905932738 },
	{ Pisometric, cinema::Vector(-SQRT1_2, 0, SQRT1_2), cinema::Vector(-SQRT1_6, 2.0 * SQRT1_6, -SQRT1_6), cinema::Vector(-SQRT1_3, -SQRT1_3, -SQRT1_3), 0.0, 0.0 },
	{ Pdimetric, cinema::Vector(-0.937086259, 0, -0.349097899), cinema::Vector(0.115059725, 0.944123613, -0.308855731),
		cinema::Vector(0.32959157, -0.32959157, -0.884725265), 0.0, 0.0 },
};

cinema::BaseObject* FindRenderCamera(cinema::BaseDocument* doc)
{
	cinema::BaseDraw* const view = doc ? doc->GetRenderBaseDraw() : nullptr;
	if (!view)
		return nullptr;
	cinema::BaseObject* const sceneCamera = view->GetSceneCamera(doc);
	return sceneCamera ? sceneCamera : view->GetEditorCamera();
}

CameraFraming CameraFraming::Full(maxon::Int32 width, maxon::Int32 height)
{
	CameraFraming framing;
	framing.width = width;
	framing.height = height;
	framing.frameWidth = maxon::Float64(width);
	framing.frameHeight = maxon::Float64(height);
	return framing;
}

CameraFraming CameraFraming::Fit(maxon::Int32 width, maxon::Int32 height, maxon::Float64 frameAspect)
{
	CameraFraming framing = Full(width, height);
	if (width < 1 || height < 1 || frameAspect <= 0.0)
		return framing;
	if (maxon::Float64(width) >= maxon::Float64(height) * frameAspect)
		framing.frameWidth = maxon::Float64(height) * frameAspect;	// image wider than the camera's picture
	else
		framing.frameHeight = maxon::Float64(width) / frameAspect;
	return framing;
}

// The camera's sensor spans the camera's picture. Blender's sensor spans the whole image
// width, so an image wider than the picture needs a sensor wider by this factor.
static maxon::Float64 SensorScale(const CameraFraming& framing)
{
	return framing.frameWidth > 0.0 ? maxon::Float64(framing.width) / framing.frameWidth : 1.0;
}

// Cinema 4D's spherical camera as Blender's equirectangular panorama [T: tests/c4d/camera_tag.py,
// against the Standard renderer]. Longitude grows towards the camera's right in both. Cinema 4D's
// latitude range is upside down against the picture: with "Fit Frame" the picture's top is
// -(Lat Min) and its bottom -(Lat Max).
static void ReadSphericalCamera(const cinema::BaseObject* camera, CameraValues& values)
{
	values.kind = CAMERAKIND::PANORAMIC;
	values.panorama = PanoramaValues();
	values.panorama.type = CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR;
	if (GetIntParameter(camera, CAMERAOBJECT_SPC_PROJECTION_MAPPING, SPHERICAL_MAPPING_LATLONG) != SPHERICAL_MAPPING_LATLONG ||
		GetIntParameter(camera, CAMERAOBJECT_SPC_GIZMO, 0) == SPHERICAL_GIZMO_DOME)
		values.notes |= CAMERANOTE::SPHERICAL_MAPPING;
	if (GetBoolParameter(camera, CAMERAOBJECT_SPC_FULL_RANGE_ENABLE, false))
		return;
	const maxon::Float64 longMin = GetFloatParameter(camera, CAMERAOBJECT_SPC_LONG_MIN, -values.panorama.longitudeMax);
	const maxon::Float64 longMax = GetFloatParameter(camera, CAMERAOBJECT_SPC_LONG_MAX, values.panorama.longitudeMax);
	const maxon::Float64 latMin = GetFloatParameter(camera, CAMERAOBJECT_SPC_LAT_MIN, -values.panorama.latitudeMax);
	const maxon::Float64 latMax = GetFloatParameter(camera, CAMERAOBJECT_SPC_LAT_MAX, values.panorama.latitudeMax);
	const maxon::Bool full = maxon::Abs(longMin + maxon::PI) < 1e-6 && maxon::Abs(longMax - maxon::PI) < 1e-6 &&
		maxon::Abs(latMin + maxon::PI05) < 1e-6 && maxon::Abs(latMax - maxon::PI05) < 1e-6;
	if (full)
		return;
	if (!GetBoolParameter(camera, CAMERAOBJECT_SPC_FIT_FRAME, false))
	{
		// Cinema 4D then draws the whole sphere and leaves what is outside the range black.
		values.notes |= CAMERANOTE::SPHERICAL_CROPPED;
		return;
	}
	values.panorama.longitudeMin = longMin;
	values.panorama.longitudeMax = longMax;
	values.panorama.latitudeMin = -latMax;
	values.panorama.latitudeMax = -latMin;
}

// The Cycles Camera tag's Lens tab.
static void ReadTagLens(const cinema::BaseContainer& data, const cinema::BaseObject* camera, CameraValues& values)
{
	const maxon::Int32 type = data.GetInt32(CYCLESCAMERA_LENS_TYPE, CYCLESCAMERA_LENS_TYPE_CAMERA);
	if (type == CYCLESCAMERA_LENS_TYPE_CAMERA)
		return;

	// The tag's lens replaces the camera's projection; the camera's own placement counts.
	values.mg = camera->GetMg();
	values.obliqueX = 0.0;
	values.obliqueY = 0.0;
	values.notes &= ~(CAMERANOTE::SPHERICAL_CROPPED | CAMERANOTE::SPHERICAL_MAPPING | CAMERANOTE::SPHERICAL_PARALLEL);
	values.clipsBehind = true;
	values.nearEnabled = GetBoolParameter(camera, CAMERAOBJECT_NEAR_CLIPPING_ENABLE, false);
	values.farEnabled = GetBoolParameter(camera, CAMERAOBJECT_FAR_CLIPPING_ENABLE, false);
	if (type == CYCLESCAMERA_LENS_TYPE_PERSPECTIVE)
	{
		values.kind = CAMERAKIND::PERSPECTIVE;
		return;
	}
	if (type == CYCLESCAMERA_LENS_TYPE_ORTHOGRAPHIC)
	{
		values.kind = CAMERAKIND::ORTHOGRAPHIC;
		values.parallelWidth = data.GetFloat(CYCLESCAMERA_LENS_ORTHO_SCALE, CAMERATAG_DEFAULT::ORTHO_SCALE);
		values.clipsBehind = values.nearEnabled;	// as Cinema 4D's parallel projection
		return;
	}

	values.kind = CAMERAKIND::PANORAMIC;
	PanoramaValues& panorama = values.panorama;
	panorama.type = data.GetInt32(CYCLESCAMERA_LENS_PANORAMA_TYPE, CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID);
	panorama.fisheyeFov = data.GetFloat(CYCLESCAMERA_LENS_FISHEYE_FOV, CAMERATAG_DEFAULT::FISHEYE_FOV);
	panorama.fisheyeLens = data.GetFloat(CYCLESCAMERA_LENS_FISHEYE_LENS, CAMERATAG_DEFAULT::FISHEYE_LENS);
	panorama.latitudeMin = data.GetFloat(CYCLESCAMERA_LENS_LATITUDE_MIN, -CAMERATAG_DEFAULT::LATITUDE);
	panorama.latitudeMax = data.GetFloat(CYCLESCAMERA_LENS_LATITUDE_MAX, CAMERATAG_DEFAULT::LATITUDE);
	panorama.longitudeMin = data.GetFloat(CYCLESCAMERA_LENS_LONGITUDE_MIN, -CAMERATAG_DEFAULT::LONGITUDE);
	panorama.longitudeMax = data.GetFloat(CYCLESCAMERA_LENS_LONGITUDE_MAX, CAMERATAG_DEFAULT::LONGITUDE);
	for (maxon::Int32 i = 0; i < 5; ++i)
		panorama.polynomial[i] = data.GetFloat(CYCLESCAMERA_LENS_POLYNOMIAL_K0 + i, CAMERATAG_DEFAULT::POLYNOMIAL[i]);
	panorama.cylinderUMin = data.GetFloat(CYCLESCAMERA_LENS_CYLINDER_U_MIN, -CAMERATAG_DEFAULT::CYLINDER_U);
	panorama.cylinderUMax = data.GetFloat(CYCLESCAMERA_LENS_CYLINDER_U_MAX, CAMERATAG_DEFAULT::CYLINDER_U);
	panorama.cylinderVMin = data.GetFloat(CYCLESCAMERA_LENS_CYLINDER_V_MIN, -CAMERATAG_DEFAULT::CYLINDER_V);
	panorama.cylinderVMax = data.GetFloat(CYCLESCAMERA_LENS_CYLINDER_V_MAX, CAMERATAG_DEFAULT::CYLINDER_V);
	panorama.cylinderRadius = data.GetFloat(CYCLESCAMERA_LENS_CYLINDER_RADIUS, CAMERATAG_DEFAULT::CYLINDER_RADIUS);
}

// The Cycles Camera tag's Depth of Field tab.
static void ReadTagDepthOfField(const cinema::BaseTag* tag, const cinema::BaseContainer& data, CameraValues& values)
{
	// A tagged camera has depth of field only while the box is checked, as Blender's camera; the
	// camera's own values and the render setting then no longer count (the maintainer, 2026-11-12).
	if (!data.GetBool(CYCLESCAMERA_DOF_ENABLE, false))
	{
		values.dof = CAMERADOF::OFF;
		return;
	}

	values.dof = CAMERADOF::ON;
	values.focusDistance = data.GetFloat(CYCLESCAMERA_DOF_FOCUS_DISTANCE, CAMERATAG_DEFAULT::FOCUS_DISTANCE);
	values.fStop = data.GetFloat(CYCLESCAMERA_DOF_FSTOP, CAMERATAG_DEFAULT::FSTOP);
	values.blades = data.GetInt32(CYCLESCAMERA_DOF_BLADES, CAMERATAG_DEFAULT::BLADES);
	values.bladeRotation = data.GetFloat(CYCLESCAMERA_DOF_ROTATION, CAMERATAG_DEFAULT::ROTATION);
	values.ratio = data.GetFloat(CYCLESCAMERA_DOF_RATIO, CAMERATAG_DEFAULT::RATIO);

	// A focus object is sent as its distance, measured as Blender and Cinema 4D both measure it:
	// along the camera's view axis. The plugin reads the scene again after every change, so the
	// distance follows a moving object in the IPR; and the object need not be a scene item.
	const cinema::BaseObject* const focus = data.GetObjectLink(CYCLESCAMERA_DOF_FOCUS_OBJECT, tag->GetDocument());
	if (focus)
	{
		const cinema::Vector axis = values.mg.sqmat.v3.GetNormalized();
		values.focusDistance = maxon::Abs(Dot(axis, focus->GetMg().off - values.mg.off));
	}
}

CameraValues CameraValues::Read(const cinema::BaseObject* camera)
{
	CameraValues values;
	values.name = camera->GetName();
	values.mg = camera->GetMg();
	values.focalLength = GetFloatParameter(camera, CAMERA_FOCUS, 36.0);
	values.sensorWidth = GetFloatParameter(camera, CAMERAOBJECT_APERTURE, 36.0);
	const maxon::Float64 zoom = GetFloatParameter(camera, CAMERA_ZOOM, 1.0);
	values.parallelWidth = PARALLEL_WIDTH_AT_ZOOM_1 / (zoom > 1e-9 ? zoom : 1e-9);
	values.filmOffsetX = GetFloatParameter(camera, CAMERAOBJECT_FILM_OFFSET_X, 0.0);
	values.filmOffsetY = GetFloatParameter(camera, CAMERAOBJECT_FILM_OFFSET_Y, 0.0);
	values.nearEnabled = GetBoolParameter(camera, CAMERAOBJECT_NEAR_CLIPPING_ENABLE, false);
	values.farEnabled = GetBoolParameter(camera, CAMERAOBJECT_FAR_CLIPPING_ENABLE, false);
	values.nearClip = GetFloatParameter(camera, CAMERAOBJECT_NEAR_CLIPPING, 0.0);
	values.farClip = GetFloatParameter(camera, CAMERAOBJECT_FAR_CLIPPING, 0.0);
	// Cinema 4D keeps the distance of the camera's own focus object here, measured along the
	// view axis, once the document is evaluated (measured: 1400 for an object 1400 ahead and 300
	// aside).
	values.focusDistance = GetFloatParameter(camera, CAMERAOBJECT_TARGETDISTANCE, 2000.0);
	values.fStop = GetFloatParameter(camera, CAMERAOBJECT_FNUMBER_VALUE, 8.0);

	// Projections, measured against the Standard renderer (docs/reference/c4d-sdk.md, "Cameras
	// and views"): perspective and parallel use the camera's rotation, and a parallel camera
	// clips only where near clipping is set; the fixed views ignore the rotation and all clipping.
	const maxon::Int32 projection = GetIntParameter(camera, CAMERA_PROJECTION, Pperspective);
	const maxon::Bool spherical = GetBoolParameter(camera, CAMERAOBJECT_SPC_ENABLE, false);
	if (projection == Pperspective)
	{
		values.kind = CAMERAKIND::PERSPECTIVE;
		if (spherical)
			ReadSphericalCamera(camera, values);
	}
	else
	{
		values.kind = CAMERAKIND::ORTHOGRAPHIC;
		values.clipsBehind = projection == Pparallel && values.nearEnabled;
		if (spherical)
			values.notes |= CAMERANOTE::SPHERICAL_PARALLEL;
		for (const FixedView& view : FIXED_VIEWS)
		{
			if (view.projection != projection)
				continue;
			values.mg = cinema::Matrix(values.mg.off, view.v1, view.v2, view.v3);
			values.obliqueX = view.slantX;
			values.obliqueY = view.slantY;
			// These views clip nothing, whatever the camera's clipping settings say.
			values.nearEnabled = false;
			values.farEnabled = false;
			break;
		}
	}

	if (const cinema::BaseTag* const tag = FindCameraTag(camera))
	{
		const cinema::BaseContainer& data = tag->GetDataInstanceRef();
		ReadTagLens(data, camera, values);
		ReadTagDepthOfField(tag, data, values);
	}
	return values;
}

maxon::Bool CameraValues::HasDepthOfField(maxon::Bool renderSetting) const
{
	if (dof == CAMERADOF::ON)
		return true;
	// Cinema 4D's own depth of field has always been translated for perspective cameras only.
	return dof == CAMERADOF::RENDER_SETTINGS && renderSetting && kind == CAMERAKIND::PERSPECTIVE;
}

// Blender maps a panorama's range onto the whole image. When the image is larger than the
// camera's picture (the IPR's "Fit Window"), the range of the two panoramas that have one grows
// with it, so the camera's picture keeps its place and the image shows more around it (latitude
// at most to the poles). The other panorama types fill the image as they are [untested].
static PanoramaValues Framed(const PanoramaValues& panorama, const CameraFraming& framing)
{
	PanoramaValues framed = panorama;
	if (framing.frameWidth <= 0.0 || framing.frameHeight <= 0.0)
		return framed;
	const maxon::Float64 wider = maxon::Float64(framing.width) / framing.frameWidth;
	const maxon::Float64 taller = maxon::Float64(framing.height) / framing.frameHeight;
	const auto grow = [](maxon::Float64& low, maxon::Float64& high, maxon::Float64 factor)
	{
		const maxon::Float64 centre = (low + high) * 0.5;
		const maxon::Float64 half = (high - low) * 0.5 * factor;
		low = centre - half;
		high = centre + half;
	};
	if (panorama.type == CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR)
	{
		grow(framed.longitudeMin, framed.longitudeMax, wider);
		grow(framed.latitudeMin, framed.latitudeMax, taller);
		framed.latitudeMin = maxon::Max(framed.latitudeMin, -maxon::PI05);
		framed.latitudeMax = maxon::Min(framed.latitudeMax, maxon::PI05);
	}
	else if (panorama.type == CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL)
	{
		grow(framed.cylinderUMin, framed.cylinderUMax, wider);
		grow(framed.cylinderVMin, framed.cylinderVMax, taller);
	}
	return framed;
}

static void WritePanorama(JsonWriter& json, const PanoramaValues& panorama)
{
	static const maxon::Char* const TYPES[] = { "EQUIRECTANGULAR", "EQUIANGULAR_CUBEMAP_FACE", "MIRRORBALL", "FISHEYE_EQUIDISTANT",
		"FISHEYE_EQUISOLID", "FISHEYE_LENS_POLYNOMIAL", "CENTRAL_CYLINDRICAL" };
	const maxon::Int32 type = maxon::ClampValue(panorama.type, maxon::Int32(0), maxon::Int32(6));
	json.BeginObject("panorama");
	json.String("panorama_type", maxon::String(TYPES[type]));
	switch (type)
	{
		case CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR:
			json.Float("latitude_min", panorama.latitudeMin);
			json.Float("latitude_max", panorama.latitudeMax);
			json.Float("longitude_min", panorama.longitudeMin);
			json.Float("longitude_max", panorama.longitudeMax);
			break;
		case CYCLESCAMERA_PANORAMA_FISHEYE_EQUIDISTANT:
			json.Float("fisheye_fov", panorama.fisheyeFov);
			break;
		case CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID:
			json.Float("fisheye_fov", panorama.fisheyeFov);
			json.Float("fisheye_lens", panorama.fisheyeLens);
			break;
		case CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL:
			json.Float("fisheye_fov", panorama.fisheyeFov);
			json.Float("fisheye_polynomial_k0", panorama.polynomial[0]);
			json.Float("fisheye_polynomial_k1", panorama.polynomial[1]);
			json.Float("fisheye_polynomial_k2", panorama.polynomial[2]);
			json.Float("fisheye_polynomial_k3", panorama.polynomial[3]);
			json.Float("fisheye_polynomial_k4", panorama.polynomial[4]);
			break;
		case CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL:
			json.Float("central_cylindrical_range_u_min", panorama.cylinderUMin);
			json.Float("central_cylindrical_range_u_max", panorama.cylinderUMax);
			json.Float("central_cylindrical_range_v_min", panorama.cylinderVMin);
			json.Float("central_cylindrical_range_v_max", panorama.cylinderVMax);
			json.Float("central_cylindrical_radius", panorama.cylinderRadius);
			break;
		default:
			break;	// the cubemap face and the mirror ball have no settings
	}
	json.EndObject();
}

// The camera's matrix as Blender gets it. For an oblique view its third axis is the direction
// the rays travel, which is not perpendicular to the picture (protocol.md, `camera`).
static cinema::Matrix ViewMatrix(const CameraValues& camera, const cinema::Matrix& mg)
{
	if (camera.kind != CAMERAKIND::ORTHOGRAPHIC || (camera.obliqueX == 0.0 && camera.obliqueY == 0.0))
		return mg;
	const cinema::Vector x = mg.sqmat.v1.GetNormalized();
	const cinema::Vector y = mg.sqmat.v2.GetNormalized();
	const cinema::Vector z = mg.sqmat.v3.GetNormalized();
	// A point drawn at (px + sx * depth, py - sy * depth) lies on a ray along (-sx, sy, 1).
	return cinema::Matrix(mg.off, x, y, z - camera.obliqueX * x + camera.obliqueY * y);
}

// Where nothing is clipped, Blender's orthographic camera stands far back along its rays.
static cinema::Matrix MovedBack(const cinema::Matrix& view, maxon::Float64 units)
{
	cinema::Matrix moved = view;
	moved.off -= view.sqmat.v3.GetNormalized() * units;
	return moved;
}

maxon::Result<void> ExportCamera(SceneWriter& writer, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, maxon::Bool eevee, const CameraFraming& framing, const cinema::Matrix* motion)
{
	const maxon::Float64 sensorScale = SensorScale(framing);
	const maxon::Float64 imageWidth = framing.width > 0 ? maxon::Float64(framing.width) : 1.0;
	const maxon::Bool orthographic = camera.kind == CAMERAKIND::ORTHOGRAPHIC;
	const maxon::Bool unclipped = orthographic && !camera.clipsBehind;
	// UNCLIPPED_DEPTH in document units.
	const maxon::Float64 back = unclipped && coords.GetScale() > 0.0 ? UNCLIPPED_DEPTH / coords.GetScale() : 0.0;

	maxon::Float64 matrix[16];
	coords.AimedMatrix(MovedBack(ViewMatrix(camera, camera.mg), back), matrix);

	if ((camera.notes & CAMERANOTE::SPHERICAL_CROPPED) != CAMERANOTE::NONE)
		writer.Warn(FormatString("Camera \"@\": the spherical camera's range without \"Fit Frame\" is not translated; the whole sphere is rendered.", camera.name));
	if ((camera.notes & CAMERANOTE::SPHERICAL_MAPPING) != CAMERANOTE::NONE)
		writer.Warn(FormatString("Camera \"@\": only the spherical camera's lat-long mapping is translated; it is rendered as lat-long.", camera.name));
	if ((camera.notes & CAMERANOTE::SPHERICAL_PARALLEL) != CAMERANOTE::NONE)
		writer.Warn(FormatString("Camera \"@\": the spherical camera works with the perspective projection only; it is rendered as a parallel view.", camera.name));
	if (eevee && camera.kind == CAMERAKIND::PANORAMIC)
		writer.Warn(FormatString("Camera \"@\": Eevee has no panoramic camera; Eevee renders it as a perspective camera. Choose Cycles to render the panorama.", camera.name));

	JsonWriter& json = writer.BeginItem("camera", "camera"_s);
	json.Floats("matrix", matrix, 16);
	if (motion)
	{
		maxon::Float64 moved[32];
		coords.AimedMatrix(MovedBack(ViewMatrix(camera, motion[0]), back), moved);
		coords.AimedMatrix(MovedBack(ViewMatrix(camera, motion[1]), back), moved + 16);
		json.Floats("motion", moved, 32);
	}
	json.String("type", camera.kind == CAMERAKIND::PANORAMIC ? "PANO"_s : orthographic ? "ORTHO"_s : "PERSP"_s);
	json.Float("lens", camera.focalLength);
	json.Float("sensor_width", camera.sensorWidth * sensorScale);
	json.Float("ortho_scale", coords.Length(camera.parallelWidth) * sensorScale);
	if (camera.kind == CAMERAKIND::PANORAMIC)
		WritePanorama(json, Framed(camera.panorama, framing));

	// Cinema 4D gives the film offset as a fraction of the width (X) and height (Y) of the
	// camera's picture, with positive Y moving the picture up. Blender's shift is a fraction
	// of the image width for both axes and moves the camera's view instead, hence the signs.
	json.Float("shift_x", camera.filmOffsetX * framing.frameWidth / imageWidth);
	json.Float("shift_y", -camera.filmOffsetY * framing.frameHeight / imageWidth);

	if (unclipped)
	{
		// Seen from the moved camera: from the camera's own plane, UNCLIPPED_DEPTH behind it, to
		// the far clipping or as far again beyond it.
		json.Float("clip_start", DEFAULT_CLIP_START);
		json.Float("clip_end", UNCLIPPED_DEPTH + (camera.farEnabled ? coords.Length(camera.farClip) : UNCLIPPED_DEPTH));
	}
	else
	{
		json.Float("clip_start", camera.nearEnabled ? maxon::Max(coords.Length(camera.nearClip), MIN_CLIP_START) : DEFAULT_CLIP_START);
		json.Float("clip_end", camera.farEnabled ? coords.Length(camera.farClip) : DEFAULT_CLIP_END);
	}

	if (camera.HasDepthOfField(depthOfField))
	{
		json.BeginObject("dof");
		// Measured from Blender's camera, which stands back where nothing is clipped.
		json.Float("focus_distance", coords.Length(camera.focusDistance) + (unclipped ? UNCLIPPED_DEPTH : 0.0));
		json.Float("fstop", camera.fStop);
		if (camera.dof == CAMERADOF::ON)
		{
			// Blender's aperture is round with fewer than three blades.
			json.Int("blades", camera.blades >= 3 ? camera.blades : 0);
			json.Float("rotation", camera.bladeRotation);
			json.Float("ratio", camera.ratio);
		}
		json.EndObject();
	}
	json.Bool("active", true);
	writer.EndItem();
	return maxon::OK;
}

maxon::Bool WriteFreeView(JsonWriter& request, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, const CameraFraming& framing)
{
	const maxon::Int32 width = framing.width;
	const maxon::Int32 height = framing.height;
	if (camera.kind != CAMERAKIND::PERSPECTIVE || camera.HasDepthOfField(depthOfField))
		return false;
	if (camera.filmOffsetX != 0.0 || camera.filmOffsetY != 0.0)
		return false;

	// The bridge turns lens and sensor into the viewport's focal length (free_view_lens in
	// bridge/c4d_bridge/viewport); the same formula here tells whether Blender will accept it.
	const maxon::Float64 sensorWidth = camera.sensorWidth * SensorScale(framing);
	if (sensorWidth <= 0.0 || width < 1 || height < 1)
		return false;
	const maxon::Float64 fit = width >= height ? 1.0 : maxon::Float64(width) / maxon::Float64(height);
	const maxon::Float64 viewportLens = 2.0 * camera.focalLength * 36.0 / sensorWidth * fit;
	if (viewportLens < 1.0 || viewportLens > 250.0)
		return false;

	maxon::Float64 matrix[16];
	coords.AimedMatrix(camera.mg, matrix);

	request.BeginObject();
	request.String("op", "view_camera"_s);
	request.Floats("matrix", matrix, 16);
	request.Float("lens", camera.focalLength);
	request.Float("sensor_width", sensorWidth);
	request.Int("width", width);
	request.Int("height", height);
	request.Float("clip_start", camera.nearEnabled ? maxon::Max(coords.Length(camera.nearClip), MIN_CLIP_START) : DEFAULT_CLIP_START);
	request.Float("clip_end", camera.farEnabled ? coords.Length(camera.farClip) : DEFAULT_CLIP_END);
	return true;
}

} // namespace cyclesrender
