// SPDX-License-Identifier: Apache-2.0
// The plugin's page in Cinema 4D's Preferences (prefs_page.h).

#include "c4d.h"
#include "c4d_resource.h"
#include "c4d_symbols.h"
#include "lib_description.h"
#include "lib_prefs.h"

#include "prefscyclesrender.h"

#include "../ids.h"
#include "devices.h"
#include "prefs_page.h"
#include "settings.h"

namespace cyclesrender
{

// The devices the page shows checkboxes for: those of the chosen type and the CPU (devices.h).
static maxon::BaseArray<CyclesDevice> ShownDevices()
{
	maxon::BaseArray<CyclesDevice> shown;
	iferr (const DeviceSettings settings = GetDeviceSettings())
		return shown;
	iferr (shown = DevicesOfType(settings, settings.type))
		return maxon::BaseArray<CyclesDevice>();
	return shown;
}

// What the page says under the button about the list.
static maxon::String DevicesStatus()
{
	iferr (const DeviceSettings settings = GetDeviceSettings())
		return maxon::String();
	if (settings.devices.IsEmpty())
		return "Not looked for yet: press Find Devices."_s;
	maxon::String text = settings.found;
	if (settings.type == DEVICETYPE::AUTOMATIC)
	{
		const DEVICETYPE resolved = ResolveDeviceType(settings);
		text += resolved == DEVICETYPE::NONE ? " No GPU: the CPU renders."_s : FormatString(" Automatic: @.", DeviceTypeName(resolved));
	}
	else if (settings.type != DEVICETYPE::NONE)
	{
		iferr (const maxon::BaseArray<CyclesDevice> shown = DevicesOfType(settings, settings.type))
			return text;
		if (shown.IsEmpty())
			text += FormatString(" No @ device: Automatic is used.", DeviceTypeName(settings.type));
	}
	return text;
}

//----------------------------------------------------------------------------------------
/// The page has no data of its own: every parameter reads from and writes to the plugin
/// settings, so the same values are available to the renderer without the page being open.
/// The device checkboxes are made here, one per device of the chosen type (GetDDescription).
//----------------------------------------------------------------------------------------
class PreferencesPage : public cinema::PrefsDialogObject
{
	INSTANCEOF(PreferencesPage, cinema::PrefsDialogObject)

public:
	static cinema::NodeData* Alloc() { return NewObjClear(PreferencesPage); }

	virtual cinema::Bool GetDDescription(const cinema::GeListNode* node, cinema::Description* description, cinema::DESCFLAGS_DESC& flags) const override
	{
		// By the description's name: the node the Preferences hand over is not of the page's type
		// (loading by node->GetType() failed, and the page fell back to its static description).
		if (!node || !description || !description->LoadDescription("Prefscyclesrender"_s))
			return false;
		flags |= cinema::DESCFLAGS_DESC::LOADED;
		const maxon::BaseArray<CyclesDevice> shown = ShownDevices();
		for (maxon::Int i = 0; i < shown.GetCount() && PREF_CYCLES_DEVICE_FIRST + i <= PREF_CYCLES_DEVICE_LAST; ++i)
		{
			cinema::BaseContainer parameter = cinema::GetCustomDataTypeDefault(cinema::DTYPE_BOOL);
			const maxon::String name = shown[i].type == "CPU"_s ? shown[i].name : FormatString("@ (@)", shown[i].name, shown[i].type);
			parameter.SetString(cinema::DESC_NAME, name);
			parameter.SetString(cinema::DESC_SHORT_NAME, name);
			parameter.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
			description->SetParameter(CreateDescID(cinema::DescLevel(PREF_CYCLES_DEVICE_FIRST + maxon::Int32(i), cinema::DTYPE_BOOL, 0)), parameter,
				CreateDescID(cinema::DescLevel(PREF_CYCLES_DEVICE_GROUP)));
		}
		return true;
	}

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

			case PREF_CYCLES_DEVICE_NOTE:
				// Short: a long text widens the page's column and pushes the fields out of sight (screenshot).
				data = cinema::GeData("What the Render Settings' device GPU uses. Blender's own preferences stay as they are."_s);
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;

			case PREF_CYCLES_DEVICE_TYPE:
			{
				iferr (const DeviceSettings settings = GetDeviceSettings())
					return false;
				data = cinema::GeData(maxon::Int32(settings.type));
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;
			}

			case PREF_CYCLES_DEVICES_STATUS:
				data = cinema::GeData(DevicesStatus());
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;

			case PREF_CYCLES_IPR_SKIP_UNCHANGED:
				data = cinema::GeData(GetIprSkipUnchangedMeshes());
				flags |= cinema::DESCFLAGS_GET::PARAM_GET;
				return true;
		}
		const maxon::Int32 index = id[0].id - PREF_CYCLES_DEVICE_FIRST;
		if (index >= 0 && id[0].id <= PREF_CYCLES_DEVICE_LAST)
		{
			const maxon::BaseArray<CyclesDevice> shown = ShownDevices();
			data = cinema::GeData(index < shown.GetCount() && shown[index].use);
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
			case PREF_CYCLES_DEVICES_STATUS:
			case PREF_CYCLES_DEVICE_NOTE:
				flags |= cinema::DESCFLAGS_SET::PARAM_SET; // Read-only.
				return true;

			case PREF_CYCLES_DEVICE_TYPE:
			{
				const maxon::Int32 type = data.GetInt32();
				if (type >= maxon::Int32(DEVICETYPE::AUTOMATIC) && type <= maxon::Int32(DEVICETYPE::METAL))
					SetDeviceType(DEVICETYPE(type));
				flags |= cinema::DESCFLAGS_SET::PARAM_SET;
				// The checkboxes follow the type.
				cinema::GeUpdateUI();
				return true;
			}

			case PREF_CYCLES_IPR_SKIP_UNCHANGED:
				SetIprSkipUnchangedMeshes(data.GetInt32() != 0);
				flags |= cinema::DESCFLAGS_SET::PARAM_SET;
				return true;
		}
		const maxon::Int32 index = id[0].id - PREF_CYCLES_DEVICE_FIRST;
		if (index >= 0 && id[0].id <= PREF_CYCLES_DEVICE_LAST)
		{
			const maxon::BaseArray<CyclesDevice> shown = ShownDevices();
			if (index < shown.GetCount())
				SetDeviceUse(shown[index].id, data.GetInt32() != 0);
			flags |= cinema::DESCFLAGS_SET::PARAM_SET;
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
			if (command->_descId[0].id == PREF_CYCLES_FIND_DEVICES)
			{
				// Asks a Blender in the background (about 2 s on the main thread: a button press).
				iferr (FindDevices())
					cinema::MessageDialog(FormatString("Cycles Render could not ask Blender for its devices: @", err.GetMessage()));
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
