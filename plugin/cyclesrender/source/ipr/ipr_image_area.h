// SPDX-License-Identifier: Apache-2.0
// The picture in the IPR window: shows the live frame with the document's view transform,
// and lets the user zoom and pan it.

#ifndef CYCLESRENDER_IPR_IMAGE_AREA_H__
#define CYCLESRENDER_IPR_IMAGE_AREA_H__

#include "c4d.h"
#include "c4d_gui.h"

#include "../session/frame_reader.h"

namespace cyclesrender
{

/// BFM_ACTION_VALUE of the action the area sends to its dialog.
enum
{
	IPR_IMAGE_ZOOM_CHANGED = 1,	///< the user zoomed with the mouse wheel
	IPR_IMAGE_CLICKED = 2				///< a plain click, without dragging
};

//----------------------------------------------------------------------------------------
/// Frames arrive as scene-linear floats in the document's render space. For display they are
/// converted with the document's OCIO view transform (or to sRGB when the document does not
/// use OCIO) into an 8-bit bitmap; the floats are kept for snapshots and saving. Scene values
/// above 32 clip to the brightest display colour.
///
/// Zoom 1 shows one render pixel per screen pixel. The view is described by the zoom and the
/// image point that sits in the middle of the area, so resizing the window keeps the picture
/// centred the same way.
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

	/// A line of text over the picture, for "starting" and error states. Empty: none.
	void SetMessage(const maxon::String& message);

	/// 1 = one render pixel per screen pixel.
	void SetZoom(maxon::Float64 zoom);
	maxon::Float64 GetZoom() const { return _zoom; }
	/// Centres the picture, at full size if that fits into the area and scaled down to fit otherwise.
	void FrameImage();

	/// A float bitmap of the camera's picture in the current frame, carrying the document's
	/// colour profiles, for the Picture Viewer and for saving. The caller owns it. Null when
	/// there is no frame.
	cinema::BaseBitmap* CreateFloatBitmap(cinema::BaseDocument* doc) const;

	/// Milliseconds the last conversion for display took.
	maxon::Float64 GetLastConvertMilliseconds() const { return _convertMs; }

private:
	void Convert(cinema::BaseDocument* doc);
	maxon::Result<void> UpdateLut(cinema::BaseDocument* doc);
	void ZoomAround(maxon::Float64 zoom, maxon::Float64 areaX, maxon::Float64 areaY);
	void NotifyParent(cinema::Int32 what);

	Frame _frame;
	cinema::BaseBitmap* _display = nullptr;	///< 8-bit, view transform applied
	maxon::BaseArray<maxon::UChar> _bytes;		///< scratch for the conversion
	cinema::OcioConverter* _converter = nullptr;
	maxon::BaseArray<cinema::Vector32> _lut;	///< the view transform sampled on a 3D grid; see Convert()
	maxon::String _lutKey;										///< the colour settings the table was built for
	maxon::String _message;

	maxon::Float64 _zoom = 1.0;
	maxon::Float64 _centerX = 0.0;	///< image point shown in the middle of the area, in render pixels
	maxon::Float64 _centerY = 0.0;
	maxon::Bool _framed = true;			///< keep the picture framed until the user zooms or pans
	maxon::Bool _fill = false;
	maxon::Float64 _cameraAspect = 0.0;
	maxon::Float64 _convertMs = 0.0;
};

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_IMAGE_AREA_H__
