// SPDX-License-Identifier: Apache-2.0
// The Cycles Object tag (object_tag.h).

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "c4d_tagdata.h"

#include "tcyclesobject.h"

#include "../ids.h"
#include "object_tag.h"

namespace cyclesrender
{

class ObjectTag : public cinema::TagData
{
	INSTANCEOF(ObjectTag, cinema::TagData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(ObjectTag); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;

		// The defaults of Blender's Principled Volume, except that a temperature grid glows
		// (blackbody intensity 1); volume_export.cpp uses the same when an object has no tag.
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		data.SetFloat(CYCLESOBJECT_VOLUME_DENSITY, 1.0);
		data.SetVector(CYCLESOBJECT_VOLUME_COLOR, cinema::Vector(0.5));
		data.SetFloat(CYCLESOBJECT_VOLUME_ANISOTROPY, 0.0);
		data.SetFloat(CYCLESOBJECT_VOLUME_EMISSION_STRENGTH, 0.0);
		data.SetVector(CYCLESOBJECT_VOLUME_EMISSION_COLOR, cinema::Vector(1.0));
		data.SetFloat(CYCLESOBJECT_VOLUME_BLACKBODY, 1.0);
		data.SetFloat(CYCLESOBJECT_VOLUME_TEMPERATURE, 1000.0);

		// Every tab starts switched off, with the values of an object without a tag.
		data.SetBool(CYCLESOBJECT_VOLUME_ENABLE, false);
		data.SetBool(CYCLESOBJECT_VISIBILITY_ENABLE, false);
		for (maxon::Int32 id : { CYCLESOBJECT_VISIBILITY_CAMERA, CYCLESOBJECT_VISIBILITY_DIFFUSE, CYCLESOBJECT_VISIBILITY_GLOSSY,
			CYCLESOBJECT_VISIBILITY_TRANSMISSION, CYCLESOBJECT_VISIBILITY_VOLUME_SCATTER, CYCLESOBJECT_VISIBILITY_SHADOW })
			data.SetBool(id, true);
		data.SetBool(CYCLESOBJECT_VISIBILITY_HOLDOUT, false);
		data.SetBool(CYCLESOBJECT_VISIBILITY_SHADOW_CATCHER, false);
		data.SetBool(CYCLESOBJECT_LINKING_ENABLE, false);
		data.SetInt32(CYCLESOBJECT_LINKING_MODE, CYCLESOBJECT_LINKING_MODE_EXCLUDE);
		data.SetBool(CYCLESOBJECT_INSTANCES_ENABLE, false);
		data.SetFloat(CYCLESOBJECT_INSTANCES_SCALE, 1.0);
		data.SetBool(CYCLESOBJECT_INSTANCES_USE_RADIUS, false);
		return true;
	}

	// The settings of a tab that is not enabled are greyed out.
	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		maxon::Int32 enable = 0;
		if (parameter >= CYCLESOBJECT_VOLUME_DENSITY && parameter < CYCLESOBJECT_VOLUME_ENABLE)
			enable = CYCLESOBJECT_VOLUME_ENABLE;
		else if (parameter > CYCLESOBJECT_VISIBILITY_ENABLE && parameter < CYCLESOBJECT_LINKING_ENABLE)
			enable = CYCLESOBJECT_VISIBILITY_ENABLE;
		else if (parameter > CYCLESOBJECT_LINKING_ENABLE && parameter < CYCLESOBJECT_INSTANCES_ENABLE)
			enable = CYCLESOBJECT_LINKING_ENABLE;
		else if (parameter > CYCLESOBJECT_INSTANCES_ENABLE && parameter < CYCLESOBJECT_GROUP_VOLUME)
			enable = CYCLESOBJECT_INSTANCES_ENABLE;
		if (item && enable && !item->GetDataInstanceRef().GetBool(enable))
			return false;
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}
};

const cinema::BaseTag* FindObjectTag(const cinema::BaseObject* op, maxon::Int32 enableId)
{
	for (; op; op = op->GetUp())
	{
		// An object may carry several of these tags; the first with the tab enabled counts.
		for (const cinema::BaseTag* tag = op->GetFirstTag(); tag; tag = tag->GetNext())
		{
			if (tag->GetType() == ID_OBJECT_TAG && tag->GetDataInstanceRef().GetBool(enableId))
				return tag;
		}
	}
	return nullptr;
}

cinema::Bool RegisterObjectTag()
{
	// The tag shows the plugin's icon, as the IPR command does; it has none of its own.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("cyclesrender.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterTagPlugin(ID_OBJECT_TAG, cinema::GeLoadString(IDS_OBJECT_TAG_NAME), TAG_VISIBLE, ObjectTag::Alloc,
		"Tcyclesobject"_s, icon, 0);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
