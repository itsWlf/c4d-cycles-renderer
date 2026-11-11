// SPDX-License-Identifier: Apache-2.0
// The plugin's page in Cinema 4D's Preferences (prefs_page.h).

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "lib_description.h"
#include "lib_prefs.h"

#include "prefscyclesrender.h"

#include "../ids.h"
#include "prefs_page.h"
#include "settings.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The page has no data of its own: every parameter reads from and writes to the plugin
/// settings, so the same values are available to the renderer without the page being open.
//----------------------------------------------------------------------------------------
class PreferencesPage : public cinema::PrefsDialogObject
{
	INSTANCEOF(PreferencesPage, cinema::PrefsDialogObject)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(PreferencesPage); }

	virtual cinema::Bool GetDParameter(const cinema::GeListNode* node, const cinema::DescID& id, cinema::GeData& data, cinema::DESCFLAGS_GET& flags) const override
	{
		switch (id[0].id)
		{
			case PREF_CYCLES_BLENDER_PATH:
				data = cinema::GeData(GetBlenderPath());
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;

			case PREF_CYCLES_BLENDER_STATUS:
				data = cinema::GeData(GetBlenderStatus());
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;
		}
		return SUPER::GetDParameter(node, id, data, flags);
	}

	virtual cinema::Bool SetDParameter(cinema::GeListNode* node, const cinema::DescID& id, const cinema::GeData& data, cinema::DESCFLAGS_SET& flags) override
	{
		switch (id[0].id)
		{
			case PREF_CYCLES_BLENDER_PATH:
				SetBlenderPath(data.GetFilename());
				flags |= cinema::DESCFLAGS_SET::PARAM_SET;
				cinema::GeUpdateUI();
				return true;

			case PREF_CYCLES_BLENDER_STATUS:
				flags |= cinema::DESCFLAGS_SET::PARAM_SET; // Read-only: derived from the path.
				return true;
		}
		return SUPER::SetDParameter(node, id, data, flags);
	}

	virtual cinema::Bool Message(cinema::GeListNode* node, cinema::Int32 type, void* data) override
	{
		if (type == MSG_DESCRIPTION_COMMAND && data)
		{
			const cinema::DescriptionCommand* const command = static_cast<const cinema::DescriptionCommand*>(data);
			if (command->_descId[0].id == PREF_CYCLES_DETECT)
			{
				if (!DetectAndStoreBlender())
					cinema::MessageDialog("No supported Blender was found. Cycles Render needs Blender 5.2; choose its blender.exe by hand."_s);
				cinema::GeUpdateUI();
				return true;
			}
		}
		return SUPER::Message(node, type, data);
	}
};

cinema::Bool RegisterPreferencesPage()
{
	return cinema::PrefsDialogObject::Register(ID_PREFERENCES, PreferencesPage::Alloc, cinema::GeLoadString(IDS_CYCLESRENDER_NAME),
		"Prefscyclesrender"_s, PREFS_RENDERER, 0) != nullptr;
}

} // namespace cyclesrender
