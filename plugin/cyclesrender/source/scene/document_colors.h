// SPDX-License-Identifier: Apache-2.0
// What a document's colour management means for the numbers Blender is given and the numbers
// that go back into Cinema 4D's buffers.

#ifndef CYCLESRENDER_DOCUMENT_COLORS_H__
#define CYCLESRENDER_DOCUMENT_COLORS_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The colour settings of a document, read once, and what they mean for the export, so that
/// Blender calculates with the numbers Cinema 4D's own renderers calculate with. Every rule
/// here is measured against the Standard renderer (docs/reference/c4d-sdk.md, "Colour
/// management"; tests/c4d/colour.py).
///
/// OCIO (the default): colour parameters are stored in the render space, which Blender is
/// told to work in (render_settings_map.cpp, BlenderWorkingSpace); Blender converts images
/// itself; the picture goes back in the render space. Nothing is converted here.
///
/// "Basic" (what the render space "Legacy (sRGB linear workflow)" of the dialog "Change Render
/// Space..." switches to; the colour management of Cinema 4D R25 and before): Blender works in
/// linear Rec.709 and is given the numbers Cinema 4D calculates with. With Linear Workflow on
/// these are linear. With it off they are the sRGB numbers themselves, which Blender treats as
/// if they were linear and which go back unchanged: light added and multiplied on sRGB numbers
/// is exactly what Cinema 4D computes then.
//----------------------------------------------------------------------------------------
class DocumentColors
{
public:
	/// OCIO, as a document that says nothing else.
	DocumentColors() = default;
	/// The settings of @p doc; OCIO for nullptr (a material preview has no document).
	explicit DocumentColors(const cinema::BaseDocument* doc);

	maxon::Bool IsOcio() const { return _ocio; }
	/// True when Cinema 4D calculates with linear values: always with OCIO, with "Basic" when
	/// Linear Workflow is on.
	maxon::Bool IsLinear() const { return _ocio || _linearWorkflow; }

	//----------------------------------------------------------------------------------------
	/// A colour parameter as Cinema 4D's renderers calculate with it: the colour of a material
	/// channel times its brightness (the brightness counts before the conversion, measured), a
	/// light's colour, a gradient knot, a shader's colour. With "Basic" this is what
	/// InitRenderStruct::TransformColor does: with Linear Workflow and the input colour profile
	/// sRGB the sRGB curve is undone (0.5 is used as 0.214); without Linear Workflow and with
	/// the profile Linear it is applied; otherwise, and always with OCIO, the colour is used
	/// as stored.
	//----------------------------------------------------------------------------------------
	cinema::Vector Input(const cinema::Vector& color) const;

	//----------------------------------------------------------------------------------------
	/// Blender's colour space for an image file of colours: the one that gives Blender the
	/// values Cinema 4D calculates with. With OCIO "sRGB", or "Linear Rec.709" for a file of
	/// linear values, as before. With "Basic" the file's values are brought from their own
	/// space into the space of the calculation: their own space is the bitmap shader's "Color
	/// Profile" when that is sRGB or Linear, and otherwise linear for .exr and .hdr and sRGB
	/// for every other file (an embedded profile changed nothing, measured); with the input
	/// colour profile Off nothing is converted.
	/// @param[in] linearFile     The file stores linear values (.exr, .hdr).
	/// @param[in] bitmapProfile  BITMAPSHADER_COLORPROFILE of the bitmap shader that reads the file, or NOTOK when there is none.
	/// @return                   nullptr when no colour space does it: linear values into a calculation on sRGB numbers
	///                           (Linear Workflow off) need the sRGB curve applied, which Blender does not do on loading.
	//----------------------------------------------------------------------------------------
	const maxon::Char* ImageColorSpace(maxon::Bool linearFile, maxon::Int32 bitmapProfile = NOTOK) const;

	//----------------------------------------------------------------------------------------
	/// "Basic" only: whether a final frame's picture goes into Cinema 4D's buffer with the sRGB
	/// curve applied. Measured: the Standard renderer does so with Linear Workflow on unless
	/// Multi-Pass is on, which makes the picture and its layers linear (the Picture Viewer then
	/// shows them through the curve). Without Linear Workflow the sRGB numbers calculated with
	/// go in as they are. A linear image colour profile in the Render Settings is taken to mean
	/// no curve either (untested).
	//----------------------------------------------------------------------------------------
	maxon::Bool CurveInPicture(const cinema::BaseContainer& renderData) const;

	/// "Basic" only: whether the screen shows the calculated numbers through the sRGB curve (as
	/// the Picture Viewer shows a final frame): with Linear Workflow on.
	maxon::Bool CurveOnScreen() const { return !_ocio && _linearWorkflow; }

private:
	maxon::Bool _ocio = true;
	maxon::Bool _linearWorkflow = true;
	maxon::Int32 _inputProfile = 0;   ///< DOCUMENT_COLORPROFILE_...
};

/// The sRGB transfer curve (IEC 61966-2-1) and its inverse, for one channel.
maxon::Float32 LinearToSrgb(maxon::Float32 value);
maxon::Float64 SrgbToLinear(maxon::Float64 value);

} // namespace cyclesrender

#endif // CYCLESRENDER_DOCUMENT_COLORS_H__
