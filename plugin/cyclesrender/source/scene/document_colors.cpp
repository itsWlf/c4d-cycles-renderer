// SPDX-License-Identifier: Apache-2.0
// The colour settings of a document and what they mean for the export (document_colors.h).

#include "c4d.h"
#include "ddoc.h"
#include "drendersettings.h"
#include "xbitmap.h"

#include "document_colors.h"

namespace cyclesrender
{

// Blender's names for the ways an image file is read.
static const maxon::Char* const COLORSPACE_SRGB = "sRGB";
static const maxon::Char* const COLORSPACE_LINEAR = "Linear Rec.709";
static const maxon::Char* const COLORSPACE_DATA = "Non-Color";

maxon::Float32 LinearToSrgb(maxon::Float32 value)
{
	if (value <= 0.0031308f)
		return value * 12.92f;
	return 1.055f * maxon::Pow(value, 1.0f / 2.4f) - 0.055f;
}

maxon::Float64 SrgbToLinear(maxon::Float64 value)
{
	if (value <= 0.04045)
		return value / 12.92;
	return maxon::Pow((value + 0.055) / 1.055, 2.4);
}

// A setting of the document as the Attribute Manager shows it; the container held all three
// in every document measured, GetParameter() is the safe way all the same (scene/parameters.h).
static cinema::GeData GetDocumentParameter(const cinema::BaseDocument* doc, maxon::Int32 id)
{
	cinema::GeData data;
	doc->GetParameter(CreateDescID(cinema::DescLevel(id)), data, cinema::DESCFLAGS_GET::NONE);
	return data;
}

DocumentColors::DocumentColors(const cinema::BaseDocument* doc)
{
	if (!doc)
		return;
	const cinema::GeData management = GetDocumentParameter(doc, cinema::DOCUMENT_COLOR_MANAGEMENT);
	_ocio = management.GetType() == cinema::DA_LONG && management.GetInt32() == cinema::DOCUMENT_COLOR_MANAGEMENT_OCIO;
	const cinema::GeData linearWorkflow = GetDocumentParameter(doc, cinema::DOCUMENT_LINEARWORKFLOW);
	_linearWorkflow = linearWorkflow.GetType() != cinema::DA_LONG || linearWorkflow.GetInt32() != 0;
	const cinema::GeData profile = GetDocumentParameter(doc, cinema::DOCUMENT_COLORPROFILE);
	_inputProfile = profile.GetType() == cinema::DA_LONG ? profile.GetInt32() : maxon::Int32(cinema::DOCUMENT_COLORPROFILE_SRGB);
}

cinema::Vector DocumentColors::Input(const cinema::Vector& color) const
{
	if (_ocio)
		return color;
	if (_linearWorkflow && _inputProfile == cinema::DOCUMENT_COLORPROFILE_SRGB)
		return cinema::Vector(SrgbToLinear(color.x), SrgbToLinear(color.y), SrgbToLinear(color.z));
	if (!_linearWorkflow && _inputProfile == cinema::DOCUMENT_COLORPROFILE_LINEAR)
		return cinema::Vector(LinearToSrgb(maxon::Float32(color.x)), LinearToSrgb(maxon::Float32(color.y)), LinearToSrgb(maxon::Float32(color.z)));
	return color;
}

const maxon::Char* DocumentColors::ImageColorSpace(maxon::Bool linearFile, maxon::Int32 bitmapProfile) const
{
	if (_ocio)
		return linearFile ? COLORSPACE_LINEAR : COLORSPACE_SRGB;
	// Measured: with the input profile Off neither the file's kind nor the bitmap's profile
	// changes what is calculated with, with and without Linear Workflow.
	if (_inputProfile == cinema::DOCUMENT_COLORPROFILE_DISABLED)
		return COLORSPACE_DATA;
	maxon::Bool sourceLinear = linearFile;
	if (bitmapProfile == BITMAPSHADER_COLORPROFILE_LINEAR)
		sourceLinear = true;
	else if (bitmapProfile == BITMAPSHADER_COLORPROFILE_SRGB)
		sourceLinear = false;
	if (sourceLinear)
		return _linearWorkflow ? COLORSPACE_LINEAR : nullptr;
	return _linearWorkflow ? COLORSPACE_SRGB : COLORSPACE_DATA;
}

maxon::Bool DocumentColors::CurveInPicture(const cinema::BaseContainer& renderData) const
{
	if (_ocio || !_linearWorkflow || renderData.GetBool(RDATA_MULTIPASS_ENABLE))
		return false;
	const cinema::ColorProfile* const profile = renderData.GetCustomDataType<cinema::ColorProfile>(RDATA_IMAGECOLORPROFILE);
	const cinema::ColorProfile* const linear = cinema::ColorProfile::GetDefaultLinearRGB();
	return !(profile && linear && *profile == *linear);
}

} // namespace cyclesrender
