// SPDX-License-Identifier: Apache-2.0
// Writes the camera a frame is rendered through as a `camera` item.

#ifndef CYCLESRENDER_CAMERA_EXPORT_H__
#define CYCLESRENDER_CAMERA_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The camera Cinema 4D would render through: the scene camera of the render view, or that
/// view's editor camera when no camera object is active. Null only for a document without views.
cinema::BaseObject* FindRenderCamera(cinema::BaseDocument* doc);

//----------------------------------------------------------------------------------------
/// What the exporter reads from a camera object. Kept apart from the object so that the IPR
/// can go on rendering through a camera as it was (its camera lock).
//----------------------------------------------------------------------------------------
struct CameraValues
{
	cinema::Matrix mg;
	maxon::Bool perspective = true;
	maxon::Float64 focalLength = 36.0;	///< millimetres
	maxon::Float64 sensorWidth = 36.0;
	maxon::Float64 zoom = 1.0;					///< of a parallel projection
	maxon::Float64 filmOffsetX = 0.0;
	maxon::Float64 filmOffsetY = 0.0;
	maxon::Bool nearEnabled = false;
	maxon::Bool farEnabled = false;
	maxon::Float64 nearClip = 0.0;			///< document units
	maxon::Float64 farClip = 0.0;
	maxon::Float64 focusDistance = 2000.0;
	maxon::Float64 fStop = 8.0;

	static CameraValues Read(const cinema::BaseObject* camera);
};

//----------------------------------------------------------------------------------------
/// Where the camera's picture lies in the rendered image. Normally it is the whole image.
/// The IPR can render an image of another shape (its window's): the camera's picture then
/// is the largest centred rectangle of the camera's aspect, and the image shows more of
/// the scene to the sides or above and below it, as Cinema 4D's views do.
//----------------------------------------------------------------------------------------
struct CameraFraming
{
	maxon::Int32 width = 0;						///< the rendered image, in pixels
	maxon::Int32 height = 0;
	maxon::Float64 frameWidth = 0.0;	///< the camera's picture inside it, in pixels
	maxon::Float64 frameHeight = 0.0;

	/// The camera's picture fills the image.
	static CameraFraming Full(maxon::Int32 width, maxon::Int32 height);
	/// The camera's picture has the aspect @p frameAspect (width / height) and is fitted into the image.
	static CameraFraming Fit(maxon::Int32 width, maxon::Int32 height, maxon::Float64 frameAspect);
};

//----------------------------------------------------------------------------------------
/// Exports @p camera as the active camera.
/// @param[in] depthOfField  Send the camera's focus distance and f-stop; otherwise everything is sharp.
/// @param[in] framing       Image size and where the camera's picture lies in it.
/// @param[in] motion        For motion blur: two matrices, the camera's global matrix when the
///                          shutter opens and when it closes; nullptr for a camera at rest.
//----------------------------------------------------------------------------------------
maxon::Result<void> ExportCamera(SceneWriter& writer, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, const CameraFraming& framing, const cinema::Matrix* motion = nullptr);

//----------------------------------------------------------------------------------------
/// For the live viewport: writes a `view_camera` request that places Blender's own view
/// like @p camera, which is much cheaper to move than a camera object (protocol.md,
/// "Viewport"). Possible for a perspective camera without film offset and without depth of
/// field whose framing fits the viewport's lens range.
/// @return  False, with nothing written, when this camera needs a real camera object.
//----------------------------------------------------------------------------------------
maxon::Bool WriteFreeView(JsonWriter& request, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, const CameraFraming& framing);

} // namespace cyclesrender

#endif // CYCLESRENDER_CAMERA_EXPORT_H__
