// SPDX-License-Identifier: Apache-2.0
// The Cycles Light tag (light_tag.h). Reading order: Init (the defaults), what the Attribute
// Manager shows (GetDDescription hides, GetDEnabling greys out), then the button that takes the
// light's own values.

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "c4d_tagdata.h"
#include "orslight.h"

#include "tcycleslight.h"

#include "../ids.h"
#include "../scene/coords.h"
#include "../scene/document_colors.h"
#include "light_settings.h"
#include "light_tag.h"

namespace cyclesrender
{

// The switch that decides for a setting, by the hundreds of its id (tcycleslight.h).
static maxon::Int32 SwitchOf(maxon::Int32 id)
{
	if (id >= CYCLESLIGHTTAG_TYPE && id < CYCLESLIGHTTAG_MAX_BOUNCES)
		return CYCLESLIGHTTAG_LIGHT_ENABLE;
	if (id >= CYCLESLIGHTTAG_MAX_BOUNCES && id < CYCLESLIGHTTAG_VISIBLE_CAMERA)
		return CYCLESLIGHTTAG_SETTINGS_ENABLE;
	if (id >= CYCLESLIGHTTAG_VISIBLE_CAMERA && id < CYCLESLIGHTTAG_EEVEE_DIFFUSE_FACTOR)
		return CYCLESLIGHTTAG_VISIBILITY_ENABLE;
	if (id >= CYCLESLIGHTTAG_EEVEE_DIFFUSE_FACTOR && id <= CYCLESLIGHTTAG_EEVEE_SHADOW_ABSOLUTE)
		return CYCLESLIGHTTAG_EEVEE_ENABLE;
	return NOTOK;
}

class LightTag : public cinema::TagData
{
	INSTANCEOF(LightTag, cinema::TagData)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(LightTag); }

	virtual cinema::Bool Init(cinema::GeListNode* node, cinema::Bool isCloneInit) override
	{
		cinema::BaseList2D* const item = static_cast<cinema::BaseList2D*>(node);
		if (!item)
			return false;
		if (isCloneInit)
			return true;
		// Every tab off: the light renders as without the tag until a switch is ticked. The values
		// are Blender's defaults, as on the Cycles Light object.
		cinema::BaseContainer& data = item->GetDataInstanceRef();
		InitLightSettings(data);
		data.SetBool(CYCLESLIGHTTAG_LIGHT_ENABLE, false);
		data.SetBool(CYCLESLIGHTTAG_SETTINGS_ENABLE, false);
		data.SetBool(CYCLESLIGHTTAG_VISIBILITY_ENABLE, false);
		data.SetBool(CYCLESLIGHTTAG_EEVEE_ENABLE, false);
		return true;
	}

	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		if (!node || !description || !description->LoadDescription(node->GetType()))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		ShowLightSettings(static_cast<const cinema::BaseList2D*>(node)->GetDataInstanceRef(), description);
		return SUPER::GetDDescription(node, description, flags);
	}

	virtual cinema::Bool GetDEnabling(const cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data,
		cinema::DESCFLAGS_ENABLE flags, const cinema::BaseContainer* itemdesc) const override
	{
		const cinema::BaseList2D* const item = static_cast<const cinema::BaseList2D*>(node);
		const maxon::Int32 parameter = id.GetDepth() > 0 ? id[0].id : 0;
		if (item)
		{
			const cinema::BaseContainer& values = item->GetDataInstanceRef();
			const maxon::Int32 owner = SwitchOf(parameter);
			if (owner != NOTOK && !values.GetBool(owner, false))
				return false;
			if (!IsLightSettingActive(values, parameter))
				return false;
		}
		return SUPER::GetDEnabling(node, id, data, flags, itemdesc);
	}

	virtual cinema::Bool Message(cinema::GeListNode* node, cinema::Int32 type, void* data) override
	{
		if (type == MSG_DESCRIPTION_COMMAND && data)
		{
			const cinema::DescriptionCommand* const command = static_cast<const cinema::DescriptionCommand*>(data);
			if (command->_descId[0].id == CYCLESLIGHTTAG_TAKE_FROM_LIGHT)
			{
				TakeFromLight(static_cast<cinema::BaseTag*>(node));
				return true;
			}
		}
		return SUPER::Message(node, type, data);
	}

private:
	// Fills the Light tab with what the light it sits on is translated to now (scene/light_export.h,
	// TranslateLight), so that ticking "Light" changes nothing until a value is changed. The colour
	// is taken as Cinema 4D stores it (the export converts the tag's colour as it does the light's).
	// The Light Multiplier of the render settings is not part of it: it applies to the tag as well.
	static void TakeFromLight(cinema::BaseTag* tag)
	{
		cinema::BaseObject* const light = tag ? tag->GetObject() : nullptr;
		cinema::BaseDocument* const doc = tag ? tag->GetDocument() : nullptr;
		if (!IsTaggableLight(light))
			return;
		const Coords coords = Coords::FromDocument(doc);
		BlenderLight values;
		if (!TranslateLight(light, coords, DocumentColors(nullptr), 1.0, values, nullptr))
			return;
		if (doc)
		{
			doc->StartUndo();
			doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, tag);
		}
		StoreLightPanel(tag->GetDataInstanceRef(), values, coords);
		if (doc)
			doc->EndUndo();
		tag->SetDirty(cinema::DIRTYFLAGS::DATA);
		cinema::EventAdd();
	}
};

maxon::Bool IsTaggableLight(const cinema::BaseObject* op)
{
	return op && (op->GetType() == Olight || op->GetType() == Orslight);
}

const cinema::BaseTag* FindLightTag(const cinema::BaseObject* light)
{
	for (const cinema::BaseTag* tag = light ? light->GetFirstTag() : nullptr; tag; tag = tag->GetNext())
	{
		if (tag->GetType() == ID_LIGHT_TAG)
			return tag;
	}
	return nullptr;
}

cinema::Bool RegisterLightTag()
{
	// A yellow icon of its own (tools/make_icons.py, light_tag), the same family as the Cycles Light
	// object's.
	cinema::BaseBitmap* icon = cinema::BaseBitmap::Alloc();
	if (icon && icon->Init(cinema::GeGetPluginResourcePath() + cinema::Filename("icons"_s) + cinema::Filename("light_tag.png"_s)) != cinema::IMAGERESULT::OK)
		cinema::BaseBitmap::Free(icon);
	const cinema::Bool registered = cinema::RegisterTagPlugin(ID_LIGHT_TAG, cinema::GeLoadString(IDS_LIGHT_TAG_NAME), TAG_VISIBLE, LightTag::Alloc,
		"Tcycleslight"_s, icon, 0);
	cinema::BaseBitmap::Free(icon);
	return registered;
}

} // namespace cyclesrender
