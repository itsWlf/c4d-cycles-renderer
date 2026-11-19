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

	// The render space by its name, as BlenderWorkingSpace (render_settings_map.cpp) tells Blender.
	_space = RENDERSPACE::REC709;
	if (_ocio)
	{
		maxon::CString render;
		maxon::CString display;
		maxon::CString view;
		maxon::CString thumbnails;
		doc->GetActiveOcioColorSpacesNames(render, display, view, thumbnails);
		const maxon::String name(render);
		maxon::Int position = 0;
		if (name.Find("ACEScg"_s, &position))
			_space = RENDERSPACE::ACESCG;
		else if (name.Find("Rec.2020"_s, &position))
			_space = RENDERSPACE::REC2020;
	}
}

DocumentColors DocumentColors::Stored(const cinema::BaseDocument* doc)
{
	DocumentColors colors(doc);
	colors._ocio = true;
	colors._linearWorkflow = true;
	return colors;
}

cinema::Vector DocumentColors::FromXyz(const cinema::Vector& xyz) const
{
	// CIE XYZ to linear Rec.709 (D65), IEC 61966-2-1.
	const cinema::Vector rec709(
		3.2409699419 * xyz.x - 1.5373831776 * xyz.y - 0.4986107603 * xyz.z,
		-0.9692436363 * xyz.x + 1.8759675015 * xyz.y + 0.0415550574 * xyz.z,
		0.0556300797 * xyz.x - 0.2039769589 * xyz.y + 1.0569715142 * xyz.z);
	switch (_space)
	{
		case RENDERSPACE::ACESCG:
			// "Linear Rec.709 (sRGB)" to ACEScg in OCIO's ACES configurations (Bradford, D65 to ACES' white).
			return cinema::Vector(
				0.6130974024 * rec709.x + 0.3395231462 * rec709.y + 0.0473794514 * rec709.z,
				0.0701937225 * rec709.x + 0.9163538791 * rec709.y + 0.0134523985 * rec709.z,
				0.0206155929 * rec709.x + 0.1095697729 * rec709.y + 0.8698146342 * rec709.z);
		case RENDERSPACE::REC2020:
			// ITU-R BT.2087, linear Rec.709 to linear Rec.2020 (the same white).
			return cinema::Vector(
				0.6274038959 * rec709.x + 0.3292830384 * rec709.y + 0.0433130657 * rec709.z,
				0.0690972894 * rec709.x + 0.9195403951 * rec709.y + 0.0113623156 * rec709.z,
				0.0163914389 * rec709.x + 0.0880133079 * rec709.y + 0.8955952532 * rec709.z);
		case RENDERSPACE::REC709:
			break;
	}
	return rec709;
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
