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

/// What kind of lens Blender gets.
enum class CAMERAKIND
{
	PERSPECTIVE,
	ORTHOGRAPHIC,
	PANORAMIC
} MAXON_ENUM_LIST(CAMERAKIND);

/// Where depth of field comes from. A camera with a Cycles Camera tag is governed by the tag's
/// checkbox "Depth of Field" alone, as Blender's camera is by its own (the maintainer, 2026-11-12).
enum class CAMERADOF
{
	RENDER_SETTINGS,	///< no tag: the camera's own focus distance and f-stop, when the render setting asks for it
	ON,								///< the tag's box checked: on, with the tag's values, whatever the render setting says
	OFF								///< the tag's box unchecked: sharp, whatever the camera and the render setting say
} MAXON_ENUM_LIST(CAMERADOF);

/// Things about a camera the exporter can only approximate; each becomes a warning.
enum class CAMERANOTE
{
	NONE = 0,
	SPHERICAL_CROPPED = 1 << 1,		///< a spherical camera's range without "Fit Frame": rendered over the full range
	SPHERICAL_MAPPING = 1 << 2,		///< a spherical camera's cubic or dome mapping: rendered as lat-long
	SPHERICAL_PARALLEL = 1 << 3		///< a spherical camera with a parallel projection: rendered as lat-long
} MAXON_ENUM_FLAGS(CAMERANOTE);

/// Blender's panorama settings (Lens panel), angles in radians. Defaults are Blender's.
struct PanoramaValues
{
	maxon::Int32 type = 4;	///< CYCLESCAMERA_PANORAMA_... of res/description/tcyclescamera.h
	maxon::Float64 fisheyeFov = 3.14159265358979323846;
	maxon::Float64 fisheyeLens = 10.5;
	maxon::Float64 latitudeMin = -1.57079632679489661923;
	maxon::Float64 latitudeMax = 1.57079632679489661923;
	maxon::Float64 longitudeMin = -3.14159265358979323846;
	maxon::Float64 longitudeMax = 3.14159265358979323846;
	maxon::Float64 polynomial[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
	maxon::Float64 cylinderUMin = -3.14159265358979323846;
	maxon::Float64 cylinderUMax = 3.14159265358979323846;
	maxon::Float64 cylinderVMin = -1.0;
	maxon::Float64 cylinderVMax = 1.0;
	maxon::Float64 cylinderRadius = 1.0;
};

//----------------------------------------------------------------------------------------
/// What the exporter reads from a camera object and its Cycles Camera tag. Kept apart from the
/// object so that the IPR can go on rendering through a camera as it was (its camera lock).
//----------------------------------------------------------------------------------------
struct CameraValues
{
	maxon::String name;
	/// Where the view is and how it is turned. For Cinema 4D's fixed views (Left to Bottom, the
	/// axonometric ones) the camera's own rotation does not count: these are the view's axes.
	cinema::Matrix mg;
	CAMERAKIND kind = CAMERAKIND::PERSPECTIVE;
	maxon::Float64 focalLength = 36.0;	///< millimetres
	maxon::Float64 sensorWidth = 36.0;
	maxon::Float64 parallelWidth = 1024.0;	///< width of an orthographic view, document units
	/// Slant of an oblique parallel projection: a point at depth z is drawn shifted by
	/// (x, y) * z across the picture. Zero for every other view.
	maxon::Float64 obliqueX = 0.0;
	maxon::Float64 obliqueY = 0.0;
	maxon::Float64 filmOffsetX = 0.0;
	maxon::Float64 filmOffsetY = 0.0;
	/// False where Cinema 4D clips nothing at all, also behind the camera: its fixed views, and a
	/// parallel camera without near clipping.
	maxon::Bool clipsBehind = true;
	maxon::Bool nearEnabled = false;
	maxon::Bool farEnabled = false;
	maxon::Float64 nearClip = 0.0;			///< document units
	maxon::Float64 farClip = 0.0;
	CAMERADOF dof = CAMERADOF::RENDER_SETTINGS;
	maxon::Float64 focusDistance = 2000.0;	///< document units, along the view axis
	maxon::Float64 fStop = 8.0;
	maxon::Int32 blades = 0;
	maxon::Float64 bladeRotation = 0.0;	///< radians
	maxon::Float64 ratio = 1.0;
	PanoramaValues panorama;
	CAMERANOTE notes = CAMERANOTE::NONE;

	/// Reads the camera, its projection and its Cycles Camera tag (tags/camera_tag.h).
	static CameraValues Read(const cinema::BaseObject* camera);

	/// Whether the frame gets depth of field.
	/// @param[in] renderSetting  The render setting "Depth of Field" (it counts unless the tag decides).
	maxon::Bool HasDepthOfField(maxon::Bool renderSetting) const;
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
	/// Width of an image pixel over its height (Cinema 4D's Pixel Aspect; Blender's pixel aspect X
	/// over Y, which the `render` request carries). The IPR renders square pixels and gives the
	/// picture the shape the pixel aspect gives it instead (CameraFraming::Fit).
	maxon::Float64 pixelAspect = 1.0;

	/// The camera's picture fills the image.
	static CameraFraming Full(maxon::Int32 width, maxon::Int32 height, maxon::Float64 pixelAspect = 1.0);
	/// The camera's picture has the aspect @p frameAspect (width / height) and is fitted into the image.
	static CameraFraming Fit(maxon::Int32 width, maxon::Int32 height, maxon::Float64 frameAspect);
};

//----------------------------------------------------------------------------------------
/// Exports @p camera as the active camera.
/// @param[in] depthOfField  The render setting "Depth of Field": send the camera's own focus
///                          distance and f-stop (a Cycles Camera tag may decide otherwise).
/// @param[in] eevee         The frame is rendered by Eevee, which has no panoramic camera: a
///                          panoramic camera is named in a warning.
/// @param[in] framing       Image size and where the camera's picture lies in it.
/// @param[in] motion        For motion blur: two matrices, the camera's view matrix (CameraValues::mg)
///                          when the shutter opens and when it closes; nullptr for a camera at rest.
//----------------------------------------------------------------------------------------
maxon::Result<void> ExportCamera(SceneWriter& writer, const Coords& coords, const CameraValues& camera,
	maxon::Bool depthOfField, maxon::Bool eevee, const CameraFraming& framing, const cinema::Matrix* motion = nullptr);

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
