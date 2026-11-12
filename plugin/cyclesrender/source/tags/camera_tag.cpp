// SPDX-License-Identifier: Apache-2.0
// The Cycles Camera tag (camera_tag.h). Reading order: Init (the defaults), then what the
// Attribute Manager shows (GetDDescription hides, GetDEnabling greys out).

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "c4d_tagdata.h"

#include "tcyclescamera.h"

#include "../ids.h"
#include "camera_tag.h"

namespace cyclesrender
{

// Which settings of the Panorama group each panorama type uses, as on Blender's Lens panel
// (scripts/startup/bl_ui/properties_data_camera.py, DATA_PT_lens).
struct PanoramaSetting
{
	maxon::Int32 id;
	maxon::Int32 types[3];	///< panorama types that show it; NOTOK fills the rest
};

static const PanoramaSetting PANORAMA_SETTINGS[] =
{
	{ CYCLESCAMERA_LENS_FISHEYE_LENS, { CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_FISHEYE_FOV, { CYCLESCAMERA_PANORAMA_FISHEYE_EQUIDISTANT, CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID,
		CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL } },
	{ CYCLESCAMERA_LENS_LATITUDE_MIN, { CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_LATITUDE_MAX, { CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_LONGITUDE_MIN, { CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_LONGITUDE_MAX, { CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_POLYNOMIAL_K0, { CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_POLYNOMIAL_K1, { CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_POLYNOMIAL_K2, { CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_POLYNOMIAL_K3, { CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_POLYNOMIAL_K4, { CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_CYLINDER_V_MIN, { CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_CYLINDER_V_MAX, { CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_CYLINDER_U_MIN, { CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_CYLINDER_U_MAX, { CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL, NOTOK, NOTOK } },
	{ CYCLESCAMERA_LENS_CYLINDER_RADIUS, { CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL, NOTOK, NOTOK } },
};

static void SetHidden(cinema::Description* description, maxon::Int32 id, maxon::Bool hidden)
{
	if (cinema::BaseContainer* const parameter = description->GetParameterI(CreateDescID(cinema::DescLevel(id)), nullptr))
		parameter->SetBool(cinema::DESC_HIDE, hidden);
}

class CameraTag : public cinema::TagData
{
	INSTANCEOF(CameraTag, cinema::TagData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(CameraTag); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;

		// Blender's defaults: depth of field unchecked, which means none for this camera (the camera's
		// own depth of field then no longer counts); the lens "as the camera".
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetBool(CYCLESCAMERA_DOF_ENABLE, false);
		data.SetFloat(CYCLESCAMERA_DOF_FOCUS_DISTANCE, CAMERATAG_DEFAULT::FOCUS_DISTANCE);
		data.SetFloat(CYCLESCAMERA_DOF_FSTOP, CAMERATAG_DEFAULT::FSTOP);
		data.SetInt32(CYCLESCAMERA_DOF_BLADES, CAMERATAG_DEFAULT::BLADES);
		data.SetFloat(CYCLESCAMERA_DOF_ROTATION, CAMERATAG_DEFAULT::ROTATION);
		data.SetFloat(CYCLESCAMERA_DOF_RATIO, CAMERATAG_DEFAULT::RATIO);

		data.SetInt32(CYCLESCAMERA_LENS_TYPE, CYCLESCAMERA_LENS_TYPE_CAMERA);
		data.SetFloat(CYCLESCAMERA_LENS_ORTHO_SCALE, CAMERATAG_DEFAULT::ORTHO_SCALE);
		// Blender's default panorama type.
		data.SetInt32(CYCLESCAMERA_LENS_PANORAMA_TYPE, CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID);
		data.SetFloat(CYCLESCAMERA_LENS_FISHEYE_FOV, CAMERATAG_DEFAULT::FISHEYE_FOV);
		data.SetFloat(CYCLESCAMERA_LENS_FISHEYE_LENS, CAMERATAG_DEFAULT::FISHEYE_LENS);
		data.SetFloat(CYCLESCAMERA_LENS_LATITUDE_MIN, -CAMERATAG_DEFAULT::LATITUDE);
		data.SetFloat(CYCLESCAMERA_LENS_LATITUDE_MAX, CAMERATAG_DEFAULT::LATITUDE);
		data.SetFloat(CYCLESCAMERA_LENS_LONGITUDE_MIN, -CAMERATAG_DEFAULT::LONGITUDE);
		data.SetFloat(CYCLESCAMERA_LENS_LONGITUDE_MAX, CAMERATAG_DEFAULT::LONGITUDE);
		for (maxon::Int32 i = 0; i < 5; ++i)
			data.SetFloat(CYCLESCAMERA_LENS_POLYNOMIAL_K0 + i, CAMERATAG_DEFAULT::POLYNOMIAL[i]);
		data.SetFloat(CYCLESCAMERA_LENS_CYLINDER_U_MIN, -CAMERATAG_DEFAULT::CYLINDER_U);
		data.SetFloat(CYCLESCAMERA_LENS_CYLINDER_U_MAX, CAMERATAG_DEFAULT::CYLINDER_U);
		data.SetFloat(CYCLESCAMERA_LENS_CYLINDER_V_MIN, -CAMERATAG_DEFAULT::CYLINDER_V);
		data.SetFloat(CYCLESCAMERA_LENS_CYLINDER_V_MAX, CAMERATAG_DEFAULT::CYLINDER_V);
		data.SetFloat(CYCLESCAMERA_LENS_CYLINDER_RADIUS, CAMERATAG_DEFAULT::CYLINDER_RADIUS);
		return true;
	}

	// The Lens tab shows what the chosen lens and panorama type use, as Blender's panel does.
	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;

		const cinema::BaseContainer& data = static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef();
		const maxon::Int32 lens = data.GetInt32(CYCLESCAMERA_LENS_TYPE, CYCLESCAMERA_LENS_TYPE_CAMERA);
		const maxon::Int32 panorama = data.GetInt32(CYCLESCAMERA_LENS_PANORAMA_TYPE, CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID);
		SetHidden(description, CYCLESCAMERA_LENS_ORTHO_SCALE, lens != CYCLESCAMERA_LENS_TYPE_ORTHOGRAPHIC);
		SetHidden(description, CYCLESCAMERA_GROUP_PANORAMA, lens != CYCLESCAMERA_LENS_TYPE_PANORAMIC);
		for (const PanoramaSetting& setting : PANORAMA_SETTINGS)
		{
			const maxon::Bool used = setting.types[0] == panorama || setting.types[1] == panorama || setting.types[2] == panorama;
			SetHidden(description, setting.id, !used);
		}
		return SUPER::GetDDescription(node, description, flags);
	}

	// The depth of field settings count only while "Depth of Field" is checked; the focus distance
	// not while a focus object is set, whose distance counts instead (the maintainer, 2026-11-12).
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (item && parameter > CYCLESCAMERA_DOF_ENABLE && parameter <= CYCLESCAMERA_DOF_RATIO)
		{
			const cinema::BaseContainer& values = item->GetDataInstanceRef();
			if (!values.GetBool(CYCLESCAMERA_DOF_ENABLE, false))
				return false;
			if (parameter == CYCLESCAMERA_DOF_FOCUS_DISTANCE && values.GetObjectLink(CYCLESCAMERA_DOF_FOCUS_OBJECT, item->GetDocument()))
				return false;
		}
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}
};

const cinema::BaseTag* FindCameraTag(const cinema::BaseObject* camera)
{
	for (const cinema::BaseTag* tag = camera ? camera->GetFirstTag() : nullptr; tag; tag = tag->GetNext())
	{
		if (tag->GetType() == ID_CAMERA_TAG)
			return tag;
	}
	return nullptr;
}

cinema::Bool RegisterCameraTag()
{
	// A grey icon of its own (tools/make_icons.py, camera_tag): the plugin's hexagon, dark, with a
	// lighter circle.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("camera_tag.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterTagPlugin(ID_CAMERA_TAG, cinema::GeLoadString(IDS_CAMERA_TAG_NAME), TAG_VISIBLE, CameraTag::Alloc,
		"Tcyclescamera"_s, icon, 0);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
