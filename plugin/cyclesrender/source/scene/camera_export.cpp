// SPDX-License-Identifier: Apache-2.0
// The camera as a `camera` item, or as a free view of Blender's viewport (camera_export.h).

#include "c4d.h"
#include "ocamera.h"

#include "camera_export.h"
#include "parameters.h"

namespace cyclesrender
{

// Without a clipping plane set on the camera, these limits (in metres) are used. Cycles does
// not care; Eevee's depth buffer loses precision when the range is much wider.
static const maxon::Float64 DEFAULT_CLIP_START = 0.01;
static const maxon::Float64 DEFAULT_CLIP_END = 10000.0;

// In parallel projection Cinema 4D shows 1024 document units across the image at zoom 1,
// whatever the resolution.
static const maxon::Float64 PARALLEL_WIDTH_AT_ZOOM_1 = 1024.0;

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

CameraValues CameraValues::Read(const cinema::BaseObject* camera)
{
	CameraValues values;
	values.mg = camera->GetMg();
	values.perspective = GetIntParameter(camera, CAMERA_PROJECTION, Pperspective) == Pperspective;
	values.focalLength = GetFloatParameter(camera, CAMERA_FOCUS, 36.0);
	values.sensorWidth = GetFloatParameter(camera, CAMERAOBJECT_APERTURE, 36.0);
	values.zoom = GetFloatParameter(camera, CAMERA_ZOOM, 1.0);
	values.filmOffsetX = GetFloatParameter(camera, CAMERAOBJECT_FILM_OFFSET_X, 0.0);
	values.filmOffsetY = GetFloatParameter(camera, CAMERAOBJECT_FILM_OFFSET_Y, 0.0);
	values.nearEnabled = GetBoolParameter(camera, CAMERAOBJECT_NEAR_CLIPPING_ENABLE, false);
	values.farEnabled = GetBoolParameter(camera, CAMERAOBJECT_FAR_CLIPPING_ENABLE, false);
	values.nearClip = GetFloatParameter(camera, CAMERAOBJECT_NEAR_CLIPPING, 0.0);
	values.farClip = GetFloatParameter(camera, CAMERAOBJECT_FAR_CLIPPING, 0.0);
	values.focusDistance = GetFloatParameter(camera, CAMERAOBJECT_TARGETDISTANCE, 2000.0);
	values.fStop = GetFloatParameter(camera, CAMERAOBJECT_FNUMBER_VALUE, 8.0);
	return values;
}

maxon::Result<void> ExportCamera(SceneWriter& writer, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, const CameraFraming& framing, const cinema::Matrix* motion)
{
	maxon::Float64 matrix[16];
	coords.AimedMatrix(camera.mg, matrix);

	const maxon::Float64 sensorScale = SensorScale(framing);
	const maxon::Float64 imageWidth = framing.width > 0 ? maxon::Float64(framing.width) : 1.0;

	JsonWriter& json = writer.BeginItem("camera", "camera"_s);
	json.Floats("matrix", matrix, 16);
	if (motion)
	{
		maxon::Float64 moved[32];
		coords.AimedMatrix(motion[0], moved);
		coords.AimedMatrix(motion[1], moved + 16);
		json.Floats("motion", moved, 32);
	}
	json.String("type", camera.perspective ? "PERSP"_s : "ORTHO"_s);
	json.Float("lens", camera.focalLength);
	json.Float("sensor_width", camera.sensorWidth * sensorScale);
	json.Float("ortho_scale", coords.Length(PARALLEL_WIDTH_AT_ZOOM_1 / (camera.zoom > 1e-9 ? camera.zoom : 1e-9)) * sensorScale);

	// Cinema 4D gives the film offset as a fraction of the width (X) and height (Y) of the
	// camera's picture, with positive Y moving the picture up. Blender's shift is a fraction
	// of the image width for both axes and moves the camera's view instead, hence the signs.
	json.Float("shift_x", camera.filmOffsetX * framing.frameWidth / imageWidth);
	json.Float("shift_y", -camera.filmOffsetY * framing.frameHeight / imageWidth);

	json.Float("clip_start", camera.nearEnabled ? coords.Length(camera.nearClip) : DEFAULT_CLIP_START);
	json.Float("clip_end", camera.farEnabled ? coords.Length(camera.farClip) : DEFAULT_CLIP_END);

	if (depthOfField && camera.perspective)
	{
		json.BeginObject("dof");
		json.Float("focus_distance", coords.Length(camera.focusDistance));
		json.Float("fstop", camera.fStop);
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
	if (!camera.perspective || depthOfField)
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
	request.Float("clip_start", camera.nearEnabled ? coords.Length(camera.nearClip) : DEFAULT_CLIP_START);
	request.Float("clip_end", camera.farEnabled ? coords.Length(camera.farClip) : DEFAULT_CLIP_END);
	return true;
}

} // namespace cyclesrender
