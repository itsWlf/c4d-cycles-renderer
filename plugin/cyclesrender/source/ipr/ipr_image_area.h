// SPDX-License-Identifier: Apache-2.0
// The picture in the IPR window: shows the live frame with the document's view transform,
// and lets the user zoom and pan it.

#ifndef CYCLESRENDER_IPR_IMAGE_AREA_H__
#define CYCLESRENDER_IPR_IMAGE_AREA_H__

#include "c4d.h"
#include "c4d_gui.h"

#include "../session/frame_reader.h"
#include "ipr_navigation.h"

namespace cyclesrender
{

/// BFM_ACTION_VALUE of the action the area sends to its dialog.
enum
{
	IPR_IMAGE_ZOOM_CHANGED = 1,	///< the user zoomed with the mouse wheel
	IPR_IMAGE_CLICKED = 2				///< a plain click, without dragging
};

//----------------------------------------------------------------------------------------
/// What a navigating drag in the picture asks of its window (ipr_navigation.h). The
/// drag is a modal loop of the picture's own (MouseDrag), in which the window's timer does not run,
/// so the window moves the camera, sends it and shows the frames that come back from inside it.
//----------------------------------------------------------------------------------------
class IprNavigationHandler
{
public:
	virtual ~IprNavigationHandler() = default;
	/// A drag that navigates begins at (@p imageX, @p imageY), in render pixels of the frame on
	/// screen, with the picture shown at @p zoom. False: it does not navigate (the window has said
	/// why), and the drag does nothing.
	virtual maxon::Bool BeginNavigation(IPRNAV mode, maxon::Float64 imageX, maxon::Float64 imageY, maxon::Float64 zoom, maxon::Int32 qualifier) = 0;
	/// The mouse went (@p dx, @p dy) screen pixels further, right and down positive.
	virtual void Navigate(maxon::Float64 dx, maxon::Float64 dy) = 0;
	/// The mouse stands still with the button held: the picture still takes new frames.
	virtual void NavigationIdle() = 0;
	/// The button was released, or Escape pressed (@p cancelled).
	virtual void EndNavigation(maxon::Bool cancelled) = 0;
};

//----------------------------------------------------------------------------------------
/// How the IPR shows a frame's values on the screen: the document's OCIO view transform, or
/// with "Basic" colour management what the Picture Viewer does with a final frame (the sRGB
/// curve with Linear Workflow on, nothing with it off; DocumentColors::CurveOnScreen). The
/// transform is sampled into a table once per colour setting (ipr_image_area.cpp). Used by
/// the window's picture, and by the test hook without a window.
//----------------------------------------------------------------------------------------
class DisplayTransform
{
	MAXON_DISALLOW_COPY_AND_ASSIGN(DisplayTransform);

public:
	DisplayTransform() = default;
	~DisplayTransform();

	/// Builds the table for @p doc's colour settings, unless it was built for these already.
	/// Without a document: the sRGB curve.
	maxon::Result<void> Update(cinema::BaseDocument* doc);
	/// One pixel's RGB values to display values 0..255. Update() first.
	void Apply(const maxon::Float32* rgb, maxon::UChar* out) const;

private:
	cinema::OcioConverter* _converter = nullptr;
	maxon::BaseArray<cinema::Vector32> _lut;	///< the transform sampled on a 3D grid
	maxon::String _key;												///< the colour settings the table was built for
};

//----------------------------------------------------------------------------------------
/// A float bitmap of the part [x0, x1) x [y0, y1) of @p frame, for the Picture Viewer and for
/// saving; the caller owns it, null when the part is empty or outside. With OCIO it holds the
/// render-space values and carries @p doc's colour profiles. With "Basic" colour management it
/// holds linear values and the linear profile, as Cinema 4D's own multi-pass render does: the
/// Picture Viewer shows them through the sRGB curve and an EXR keeps them as they are (what
/// Cinema 4D writes for a final frame, measured); without Linear Workflow the frame's sRGB
/// numbers are linearised for that. Used by the window (IprImageArea::CreateFloatBitmap) and
/// by the test hook.
//----------------------------------------------------------------------------------------
cinema::BaseBitmap* CreateFrameBitmap(const Frame& frame, maxon::Int32 x0, maxon::Int32 y0, maxon::Int32 x1, maxon::Int32 y1, cinema::BaseDocument* doc);

//----------------------------------------------------------------------------------------
/// Frames arrive as scene-linear floats in the document's render space (with "Basic" colour
/// management: the numbers Cinema 4D calculates with). For display they are converted by a
/// DisplayTransform into an 8-bit bitmap; the floats are kept for snapshots and saving. Scene
/// values above 32 clip to the brightest display colour.
///
/// Zoom 1 shows one render pixel per screen pixel. The view is described by the zoom and the
/// image point that sits in the middle of the area, so resizing the window keeps the picture
/// centred the same way. The zoom is always a whole number of tenths of a percent (0.473, not
/// 0.4729), whichever way it was set, so that the window's zoom field can show it exactly.
///
/// A frame can be larger than the camera's picture (the IPR's "Fit Window"). The part
/// outside the camera's picture is shown darkened, like the border of Cinema 4D's views, and
/// is left out of snapshots and saved images.
//----------------------------------------------------------------------------------------
class IprImageArea : public cinema::GeUserArea
{
public:
	IprImageArea();
	virtual ~IprImageArea();

	virtual cinema::Bool GetMinSize(cinema::Int32& w, cinema::Int32& h) override;
	virtual void Sized(cinema::Int32 w, cinema::Int32 h) override;
	virtual void DrawMsg(cinema::Int32 x1, cinema::Int32 y1, cinema::Int32 x2, cinema::Int32 y2, const cinema::BaseContainer& msg) override;
	virtual cinema::Bool InputEvent(const cinema::BaseContainer& msg) override;

	/// Shows @p frame and redraws. The two swap their pixel buffers, so @p frame afterwards
	/// holds the previous picture's memory for reuse. @p doc supplies the view transform.
	/// @param[in] cameraAspect  Width / height of the camera's picture, which is fitted into the
	///                          frame and centred; 0 when the frame is the camera's picture.
	void SetFrame(Frame& frame, cinema::BaseDocument* doc, maxon::Float64 cameraAspect = 0.0);
	const Frame& GetFrame() const { return _frame; }
	/// The camera's picture inside the frame, in frame pixels: left, top, right, bottom (right and bottom exclusive).
	void GetCameraRect(maxon::Int32& x0, maxon::Int32& y0, maxon::Int32& x1, maxon::Int32& y1) const;
	/// While framed, a picture smaller than the area is enlarged to fill it. For "Fit Window",
	/// where the picture is about to be rendered again at the area's size.
	void SetFill(maxon::Bool fill);

	/// A text over the picture, for "starting" and error states, wrapped to the area's width; a "\n"
	/// starts a new line. Empty: none.
	void SetMessage(const maxon::String& message);
	const maxon::String& GetMessage() const { return _message; }

	/// Who moves the camera when a drag navigates (NavigationForDrag); nullptr: no drag navigates.
	void SetNavigationHandler(IprNavigationHandler* handler) { _navigation = handler; }

	/// 1 = one render pixel per screen pixel. Rounded to 0.1 % and kept between 2 % and 6400 %;
	/// GetZoom() returns what was made of it.
	void SetZoom(maxon::Float64 zoom);
	maxon::Float64 GetZoom() const { return _zoom; }
	/// The image point (render pixels) shown in the middle of the area.
	void GetCenter(maxon::Float64& x, maxon::Float64& y) const { x = _centerX; y = _centerY; }
	/// Centres the picture, at full size if that fits into the area and scaled down to fit otherwise.
	void FrameImage();

	/// A float bitmap of the camera's picture in the current frame, for the Picture Viewer and
	/// for saving (CreateFrameBitmap). The caller owns it. Null when there is no frame.
	cinema::BaseBitmap* CreateFloatBitmap(cinema::BaseDocument* doc) const;

	/// Milliseconds the last conversion for display took.
	maxon::Float64 GetLastConvertMilliseconds() const { return _convertMs; }

private:
	void Convert(cinema::BaseDocument* doc);
	void ZoomAround(maxon::Float64 zoom, maxon::Float64 areaX, maxon::Float64 areaY);
	void NotifyParent(cinema::Int32 what);
	/// The message as lines no wider than @p width pixels (whole words; a longer word stands alone).
	maxon::Result<void> WrapMessage(cinema::Int32 width, maxon::BaseArray<maxon::String>& lines);
	/// The modal loop of a navigating drag that began at (@p mouseX, @p mouseY), area pixels.
	void NavigationDrag(IPRNAV mode, cinema::Int32 channel, cinema::Int32 mouseX, cinema::Int32 mouseY, cinema::Int32 qualifier);

	IprNavigationHandler* _navigation = nullptr;

	Frame _frame;
	cinema::BaseBitmap* _display = nullptr;	///< 8-bit, view transform applied
	maxon::BaseArray<maxon::UChar> _bytes;		///< scratch for the conversion
	DisplayTransform _transform;
	maxon::String _message;

	maxon::Float64 _zoom = 1.0;
	maxon::Float64 _wheelZoom = 1.0;	///< the zoom the wheel aims at, before rounding; see InputEvent()
	maxon::Float64 _centerX = 0.0;	///< image point shown in the middle of the area, in render pixels
	maxon::Float64 _centerY = 0.0;
	maxon::Bool _framed = true;			///< keep the picture framed until the user zooms or pans
	maxon::Bool _fill = false;
	maxon::Float64 _cameraAspect = 0.0;
	maxon::Float64 _convertMs = 0.0;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_IMAGE_AREA_H__
