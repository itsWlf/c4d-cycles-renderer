// SPDX-License-Identifier: Apache-2.0
// The picture area of the IPR window: display conversion, zoom and pan (ipr_image_area.h).

#include "c4d.h"
#include "c4d_colors.h"
#include "ddoc.h"
#include "maxon/gfx_image_colorprofile.h"
#include "maxon/parallelfor.h"

#include "../scene/camera_export.h"
#include "ipr_image_area.h"

namespace cyclesrender
{

static const maxon::Float64 MIN_ZOOM = 0.02;
static const maxon::Float64 MAX_ZOOM = 64.0;
static const maxon::Float64 WHEEL_STEP = 1.15;	// zoom factor per wheel notch

// Brightness left outside the camera's picture, in 1/256.
static const maxon::Int BORDER_BRIGHTNESS = 115;

// The sRGB transfer curve, for documents without OCIO colour management.
static inline maxon::Float32 LinearToSrgb(maxon::Float32 value)
{
	if (value <= 0.0031308f)
		return value * 12.92f;
	return 1.055f * maxon::Pow(value, 1.0f / 2.4f) - 0.055f;
}

static inline maxon::UChar ToByte(maxon::Float32 value)
{
	if (!(value > 0.0f))	// also catches NaN
		return 0;
	if (value >= 1.0f)
		return 255;
	return maxon::UChar(value * 255.0f + 0.5f);
}

IprImageArea::IprImageArea()
{
}

IprImageArea::~IprImageArea()
{
	cinema::BaseBitmap::Free(_display);
	if (_converter)
		DeleteObj(_converter);
}

cinema::Bool IprImageArea::GetMinSize(cinema::Int32& w, cinema::Int32& h)
{
	w = 160;
	h = 90;
	return true;
}

void IprImageArea::Sized(cinema::Int32 w, cinema::Int32 h)
{
	if (_framed)
		FrameImage();
}

void IprImageArea::NotifyParent(cinema::Int32 what)
{
	cinema::BaseContainer action(cinema::BFM_ACTION);
	action.SetInt32(cinema::BFM_ACTION_ID, GetId());
	action.SetInt32(cinema::BFM_ACTION_VALUE, what);
	SendParentMessage(action);
}

// --- frames ---------------------------------------------------------------------------------

void IprImageArea::GetCameraRect(maxon::Int32& x0, maxon::Int32& y0, maxon::Int32& x1, maxon::Int32& y1) const
{
	const CameraFraming framing = CameraFraming::Fit(_frame.width, _frame.height, _cameraAspect);
	const maxon::Int32 width = maxon::ClampValue(maxon::Int32(framing.frameWidth + 0.5), 1, maxon::Max(maxon::Int32(1), _frame.width));
	const maxon::Int32 height = maxon::ClampValue(maxon::Int32(framing.frameHeight + 0.5), 1, maxon::Max(maxon::Int32(1), _frame.height));
	x0 = (_frame.width - width) / 2;
	y0 = (_frame.height - height) / 2;
	x1 = x0 + width;
	y1 = y0 + height;
}

void IprImageArea::SetFill(maxon::Bool fill)
{
	if (fill == _fill)
		return;
	_fill = fill;
	if (_framed)
		FrameImage();
}

void IprImageArea::SetFrame(Frame& frame, cinema::BaseDocument* doc, maxon::Float64 cameraAspect)
{
	const maxon::Bool sizeChanged = frame.width != _frame.width || frame.height != _frame.height;
	std::swap(_frame, frame);
	_cameraAspect = cameraAspect;
	Convert(doc);
	if (sizeChanged && _framed)
		FrameImage();
	else
		Redraw();
}

// The view transform is an arbitrary colour-to-colour function and far too slow to call per
// pixel (measured: 78 ms for 640 x 360). It is sampled once into a 3D table over a logarithmic
// scale of scene values and looked up with trilinear interpolation.
static const maxon::Int LUT_SIZE = 48;
static const maxon::Float32 LUT_SCALE = 512.0f;					// resolution near black
static const maxon::Float32 LUT_MAX_VALUE = 32.0f;				// brighter scene values clip
static const maxon::Float32 LUT_LOG_RANGE = 13.9999120f;	// log2(LUT_MAX_VALUE * LUT_SCALE + 1)

// Scene value -> position 0..1 along one axis of the table.
static inline maxon::Float32 LutPosition(maxon::Float32 value)
{
	if (!(value > 0.0f))	// also catches NaN
		return 0.0f;
	if (value >= LUT_MAX_VALUE)
		return 1.0f;
	return maxon::Log2(value * LUT_SCALE + 1.0f) / LUT_LOG_RANGE;
}

maxon::Result<void> IprImageArea::UpdateLut(cinema::BaseDocument* doc)
{
	iferr_scope;

	cinema::GeData management;
	const maxon::Bool ocio = doc && doc->GetParameter(ConstDescIDLevel(cinema::DOCUMENT_COLOR_MANAGEMENT), management, cinema::DESCFLAGS_GET::NONE) &&
		management.GetInt32() == cinema::DOCUMENT_COLOR_MANAGEMENT_OCIO;

	// Rebuild only when the document's colour settings changed.
	maxon::String key = "srgb"_s;
	if (ocio)
	{
		maxon::CString render;
		maxon::CString display;
		maxon::CString view;
		maxon::CString thumbnails;
		doc->GetActiveOcioColorSpacesNames(render, display, view, thumbnails);
		key = FormatString("@|@|@", render, display, view);
	}
	if (key == _lutKey && _lut.GetCount() == LUT_SIZE * LUT_SIZE * LUT_SIZE)
		return maxon::OK;

	_lut.Resize(LUT_SIZE * LUT_SIZE * LUT_SIZE) iferr_return;
	maxon::Float32 axis[LUT_SIZE];
	for (maxon::Int i = 0; i < LUT_SIZE; ++i)
		axis[i] = (maxon::Pow(2.0f, maxon::Float32(i) / maxon::Float32(LUT_SIZE - 1) * LUT_LOG_RANGE) - 1.0f) / LUT_SCALE;
	maxon::Int index = 0;
	for (maxon::Int b = 0; b < LUT_SIZE; ++b)
	{
		for (maxon::Int g = 0; g < LUT_SIZE; ++g)
		{
			for (maxon::Int r = 0; r < LUT_SIZE; ++r)
				_lut[index++] = cinema::Vector32(axis[r], axis[g], axis[b]);
		}
	}

	if (ocio)
	{
		iferr (_converter = cinema::OcioConverter::Init(doc, _converter))
			_converter = nullptr;
	}
	if (ocio && _converter)
	{
		_converter->TransformColors(_lut.GetFirst(), _lut.GetCount(), cinema::COLORSPACETRANSFORMATION::OCIO_RENDERING_TO_VIEW);
	}
	else
	{
		for (cinema::Vector32& color : _lut)
			color = cinema::Vector32(LinearToSrgb(color.x), LinearToSrgb(color.y), LinearToSrgb(color.z));
	}
	_lutKey = key;
	return maxon::OK;
}

void IprImageArea::Convert(cinema::BaseDocument* doc)
{
	const maxon::Int32 width = _frame.width;
	const maxon::Int32 height = _frame.height;
	if (width < 1 || height < 1)
		return;
	const maxon::Float64 started = cinema::GeGetMilliSeconds();

	if (!_display || _display->GetBw() != width || _display->GetBh() != height)
	{
		cinema::BaseBitmap::Free(_display);
		_display = cinema::BaseBitmap::Alloc();
		if (!_display || _display->Init(width, height, 24) != cinema::IMAGERESULT::OK)
		{
			cinema::BaseBitmap::Free(_display);
			return;
		}
	}
	iferr (_bytes.Resize(maxon::Int(width) * height * 3, maxon::COLLECTION_RESIZE_FLAGS::POD_UNINITIALIZED))
		return;
	iferr (UpdateLut(doc))
		return;

	const maxon::Float32* const source = _frame.pixels.GetFirst();
	maxon::UChar* const target = _bytes.GetFirst();
	const cinema::Vector32* const lut = _lut.GetFirst();
	maxon::Int32 cameraX0, cameraY0, cameraX1, cameraY1;
	GetCameraRect(cameraX0, cameraY0, cameraX1, cameraY1);
	auto convertRow = [source, target, width, lut, cameraX0, cameraY0, cameraX1, cameraY1](maxon::Int y)
	{
		const maxon::Float32* in = source + y * width * 4;
		maxon::UChar* out = target + y * width * 3;
		const maxon::Float32 last = maxon::Float32(LUT_SIZE - 1);
		for (maxon::Int x = 0; x < width; ++x, in += 4, out += 3)
		{
			const maxon::Float32 pr = LutPosition(in[0]) * last;
			const maxon::Float32 pg = LutPosition(in[1]) * last;
			const maxon::Float32 pb = LutPosition(in[2]) * last;
			const maxon::Int ir = maxon::Min(maxon::Int(pr), LUT_SIZE - 2);
			const maxon::Int ig = maxon::Min(maxon::Int(pg), LUT_SIZE - 2);
			const maxon::Int ib = maxon::Min(maxon::Int(pb), LUT_SIZE - 2);
			const maxon::Float32 fr = pr - maxon::Float32(ir);
			const maxon::Float32 fg = pg - maxon::Float32(ig);
			const maxon::Float32 fb = pb - maxon::Float32(ib);
			const cinema::Vector32* const cell = lut + (ib * LUT_SIZE + ig) * LUT_SIZE + ir;
			const maxon::Int stepG = LUT_SIZE;
			const maxon::Int stepB = LUT_SIZE * LUT_SIZE;
			const cinema::Vector32 c00 = cell[0] + (cell[1] - cell[0]) * fr;
			const cinema::Vector32 c10 = cell[stepG] + (cell[stepG + 1] - cell[stepG]) * fr;
			const cinema::Vector32 c01 = cell[stepB] + (cell[stepB + 1] - cell[stepB]) * fr;
			const cinema::Vector32 c11 = cell[stepB + stepG] + (cell[stepB + stepG + 1] - cell[stepB + stepG]) * fr;
			const cinema::Vector32 c0 = c00 + (c10 - c00) * fg;
			const cinema::Vector32 c1 = c01 + (c11 - c01) * fg;
			const cinema::Vector32 color = c0 + (c1 - c0) * fb;
			out[0] = ToByte(color.x);
			out[1] = ToByte(color.y);
			out[2] = ToByte(color.z);
		}
		// Outside the camera's picture: darkened.
		maxon::UChar* const row = target + y * width * 3;
		const maxon::Bool inside = y >= cameraY0 && y < cameraY1;
		const maxon::Int leftEnd = inside ? maxon::Int(cameraX0) * 3 : maxon::Int(width) * 3;
		for (maxon::Int i = 0; i < leftEnd; ++i)
			row[i] = maxon::UChar((row[i] * BORDER_BRIGHTNESS) >> 8);
		for (maxon::Int i = inside ? maxon::Int(cameraX1) * 3 : maxon::Int(width) * 3; i < maxon::Int(width) * 3; ++i)
			row[i] = maxon::UChar((row[i] * BORDER_BRIGHTNESS) >> 8);
	};
	maxon::ParallelFor::Dynamic(0, maxon::Int(height), convertRow);

	for (maxon::Int32 y = 0; y < height; ++y)
		_display->SetPixelCnt(0, y, width, target + maxon::Int(y) * width * 3, 3, cinema::COLORMODE::RGB, cinema::PIXELCNT::NONE);

	_convertMs = cinema::GeGetMilliSeconds() - started;
}

cinema::BaseBitmap* IprImageArea::CreateFloatBitmap(cinema::BaseDocument* doc) const
{
	if (_frame.width < 1 || _frame.height < 1)
		return nullptr;
	maxon::Int32 x0, y0, x1, y1;
	GetCameraRect(x0, y0, x1, y1);
	const maxon::Int32 width = x1 - x0;
	const maxon::Int32 height = y1 - y0;

	cinema::BaseBitmap* bitmap = cinema::BaseBitmap::Alloc();
	if (!bitmap || bitmap->Init(width, height, 96) != cinema::IMAGERESULT::OK)
	{
		cinema::BaseBitmap::Free(bitmap);
		return nullptr;
	}
	for (maxon::Int32 y = 0; y < height; ++y)
	{
		// 16 bytes from one pixel to the next: the alpha value is skipped.
		bitmap->SetPixelCnt(0, y, width, reinterpret_cast<maxon::UChar*>(const_cast<maxon::Float32*>(_frame.pixels.GetFirst() + (maxon::Int(y + y0) * _frame.width + x0) * 4)),
			16, cinema::COLORMODE::RGBf, cinema::PIXELCNT::NONE);
	}

	// With the document's profiles the Picture Viewer shows the pixels as the viewport would.
	if (doc)
	{
		maxon::ColorProfile renderSpace;
		maxon::ColorProfile displaySpace;
		maxon::ColorProfile viewTransform;
		maxon::ColorProfile thumbnails;
		doc->GetOcioProfiles(renderSpace, displaySpace, viewTransform, thumbnails);
		const maxon::ColorProfile* const profiles[3] = { &renderSpace, &viewTransform, &displaySpace };
		const maxon::Int32 indices[3] = { cinema::BaseBitmap::COLORPROFILE_INDEX_RENDERSPACE, cinema::BaseBitmap::COLORPROFILE_INDEX_VIEW_TRANSFORM,
			cinema::BaseBitmap::COLORPROFILE_INDEX_DISPLAYSPACE };
		for (maxon::Int i = 0; i < 3; ++i)
		{
			cinema::ColorProfile* profile = cinema::ColorProfile::Alloc(*profiles[i]);
			if (profile)
			{
				bitmap->SetColorProfile(profile, indices[i]);
				cinema::ColorProfile::Free(profile);
			}
		}
		cinema::ColorProfile* image = cinema::ColorProfile::Alloc(renderSpace);
		if (image)
		{
			bitmap->SetColorProfile(image, cinema::BaseBitmap::COLORPROFILE_INDEX_IMAGE);
			cinema::ColorProfile::Free(image);
		}
	}
	return bitmap;
}

void IprImageArea::SetMessage(const maxon::String& message)
{
	if (message == _message)
		return;
	_message = message;
	Redraw();
}

// --- view -----------------------------------------------------------------------------------

void IprImageArea::SetZoom(maxon::Float64 zoom)
{
	_zoom = maxon::ClampValue(zoom, MIN_ZOOM, MAX_ZOOM);
	_framed = false;
	Redraw();
}

void IprImageArea::FrameImage()
{
	_framed = true;
	if (_frame.width > 0 && _frame.height > 0 && GetWidth() > 0 && GetHeight() > 0)
	{
		const maxon::Float64 fit = maxon::Min(maxon::Float64(GetWidth()) / _frame.width, maxon::Float64(GetHeight()) / _frame.height);
		_zoom = maxon::ClampValue(_fill ? fit : maxon::Min(fit, 1.0), MIN_ZOOM, MAX_ZOOM);
		_centerX = _frame.width * 0.5;
		_centerY = _frame.height * 0.5;
	}
	Redraw();
}

void IprImageArea::ZoomAround(maxon::Float64 zoom, maxon::Float64 areaX, maxon::Float64 areaY)
{
	zoom = maxon::ClampValue(zoom, MIN_ZOOM, MAX_ZOOM);
	// The image point under the cursor stays under the cursor.
	const maxon::Float64 offsetX = areaX - GetWidth() * 0.5;
	const maxon::Float64 offsetY = areaY - GetHeight() * 0.5;
	const maxon::Float64 imageX = _centerX + offsetX / _zoom;
	const maxon::Float64 imageY = _centerY + offsetY / _zoom;
	_zoom = zoom;
	_centerX = imageX - offsetX / zoom;
	_centerY = imageY - offsetY / zoom;
	_framed = false;
	Redraw();
}

void IprImageArea::DrawMsg(cinema::Int32 x1, cinema::Int32 y1, cinema::Int32 x2, cinema::Int32 y2, const cinema::BaseContainer& msg)
{
	OffScreenOn();
	const cinema::Int32 areaWidth = GetWidth();
	const cinema::Int32 areaHeight = GetHeight();
	DrawSetPen(cinema::Vector(0.0));
	DrawRectangle(0, 0, areaWidth, areaHeight);

	if (_display && _frame.width > 0 && _frame.height > 0)
	{
		// Draw only the part of the image that is in view, in whole image pixels, so that a
		// strongly magnified picture does not ask for a destination far larger than the area.
		const maxon::Float64 halfW = areaWidth * 0.5;
		const maxon::Float64 halfH = areaHeight * 0.5;
		const maxon::Int32 sx0 = maxon::ClampValue(maxon::Int32(maxon::Floor(_centerX - halfW / _zoom)), 0, _frame.width);
		const maxon::Int32 sy0 = maxon::ClampValue(maxon::Int32(maxon::Floor(_centerY - halfH / _zoom)), 0, _frame.height);
		const maxon::Int32 sx1 = maxon::ClampValue(maxon::Int32(maxon::Ceil(_centerX + halfW / _zoom)), 0, _frame.width);
		const maxon::Int32 sy1 = maxon::ClampValue(maxon::Int32(maxon::Ceil(_centerY + halfH / _zoom)), 0, _frame.height);
		if (sx1 > sx0 && sy1 > sy0)
		{
			const maxon::Int32 dx0 = maxon::Int32(maxon::Floor(halfW + (sx0 - _centerX) * _zoom + 0.5));
			const maxon::Int32 dy0 = maxon::Int32(maxon::Floor(halfH + (sy0 - _centerY) * _zoom + 0.5));
			const maxon::Int32 dx1 = maxon::Int32(maxon::Floor(halfW + (sx1 - _centerX) * _zoom + 0.5));
			const maxon::Int32 dy1 = maxon::Int32(maxon::Floor(halfH + (sy1 - _centerY) * _zoom + 0.5));
			// Sampled scaling when the picture is shown smaller; plain pixels at 100 % and above.
			const cinema::Int32 mode = _zoom < 0.999 ? cinema::BMP_NORMALSCALED : cinema::BMP_NORMAL;
			DrawBitmap(_display, dx0, dy0, dx1 - dx0, dy1 - dy0, sx0, sy0, sx1 - sx0, sy1 - sy0, mode);
		}
	}

	if (!_message.IsEmpty())
	{
		// On a dark plate, so the text reads on any picture.
		const cinema::Int32 textWidth = DrawGetTextWidth(_message);
		const cinema::Int32 textHeight = DrawGetFontHeight();
		const cinema::Int32 left = (areaWidth - textWidth) / 2;
		const cinema::Int32 top = (areaHeight - textHeight) / 2;
		const cinema::Vector plate(0.07);
		DrawSetPen(plate);
		DrawRectangle(left - 14, top - 9, left + textWidth + 14, top + textHeight + 9);
		DrawSetTextCol(cinema::Vector(0.9), plate);
		DrawText(_message, left, top);
	}
}

cinema::Bool IprImageArea::InputEvent(const cinema::BaseContainer& msg)
{
	if (msg.GetInt32(cinema::BFM_INPUT_DEVICE) != cinema::BFM_INPUT_MOUSE)
		return false;

	const cinema::Int32 channel = msg.GetInt32(cinema::BFM_INPUT_CHANNEL);
	cinema::Int32 mouseX = msg.GetInt32(cinema::BFM_INPUT_X);
	cinema::Int32 mouseY = msg.GetInt32(cinema::BFM_INPUT_Y);
	Global2Local(&mouseX, &mouseY);

	if (channel == cinema::BFM_INPUT_MOUSEWHEEL)
	{
		const maxon::Float64 notches = msg.GetFloat(cinema::BFM_INPUT_VSCROLL) / 120.0;
		ZoomAround(_zoom * maxon::Pow(WHEEL_STEP, notches), mouseX, mouseY);
		NotifyParent(IPR_IMAGE_ZOOM_CHANGED);
		return true;
	}

	if (channel == cinema::BFM_INPUT_MOUSELEFT || channel == cinema::BFM_INPUT_MOUSEMIDDLE)
	{
		// Drag to pan.
		maxon::Float dragX = 0.0;
		maxon::Float dragY = 0.0;
		maxon::Float64 moved = 0.0;
		cinema::BaseContainer channels;
		MouseDragStart(channel, maxon::Float(mouseX), maxon::Float(mouseY), cinema::MOUSEDRAGFLAGS::DONTHIDEMOUSE);
		while (MouseDrag(&dragX, &dragY, &channels) == cinema::MOUSEDRAGRESULT::CONTINUE)
		{
			if (dragX == 0.0 && dragY == 0.0)
				continue;
			moved += maxon::Abs(dragX) + maxon::Abs(dragY);
			// MouseDrag() reports how far the mouse went against the drag's direction, so the
			// distance is added: with "-=" the picture runs away from the mouse.
			_centerX += dragX / _zoom;
			_centerY += dragY / _zoom;
			_framed = false;
			Redraw();
		}
		MouseDragEnd();
		if (moved < 2.0)
			NotifyParent(IPR_IMAGE_CLICKED);
		return true;
	}
	return false;
}

} // namespace cyclesrender
