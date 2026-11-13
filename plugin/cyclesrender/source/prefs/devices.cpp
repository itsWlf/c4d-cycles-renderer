// SPDX-License-Identifier: Apache-2.0
// The Cycles render devices of the plugin's preferences (devices.h). Reading order: the storage
// in Cinema 4D's preferences, asking a Blender for its devices, the fields of a settings request.

#include "c4d.h"

#include "../ids.h"
#include "../session/bridge_client.h"
#include "../session/json.h"
#include "devices.h"

namespace cyclesrender
{

// Keys inside the plugin's container in Cinema 4D's preferences (settings.cpp has the others,
// 1000 and 1001). Never renumber.
enum
{
	SETTING_DEVICE_TYPE = 1010,
	SETTING_DEVICES = 1011,      // a container of containers, one per device, keys below
	SETTING_DEVICES_FOUND = 1012
};
enum
{
	DEVICE_ID = 1,
	DEVICE_NAME = 2,
	DEVICE_TYPE = 3,
	DEVICE_USE = 4
};

static const maxon::Char* const BACKENDS[] = { "OPTIX", "CUDA", "HIP", "ONEAPI", "METAL" };

static cinema::BaseContainer ReadSettings()
{
	const cinema::BaseContainer* const stored = cinema::GetWorldPluginData(ID_CYCLES_VIDEOPOST);
	return stored ? *stored : cinema::BaseContainer();
}

maxon::String DeviceTypeName(DEVICETYPE type)
{
	switch (type)
	{
		case DEVICETYPE::NONE: return "NONE"_s;
		case DEVICETYPE::OPTIX: return "OPTIX"_s;
		case DEVICETYPE::CUDA: return "CUDA"_s;
		case DEVICETYPE::HIP: return "HIP"_s;
		case DEVICETYPE::ONEAPI: return "ONEAPI"_s;
		case DEVICETYPE::METAL: return "METAL"_s;
		case DEVICETYPE::AUTOMATIC: break;
	}
	return maxon::String();
}

static DEVICETYPE TypeOfName(const maxon::String& name)
{
	for (maxon::Int i = 0; i < 5; ++i)
	{
		if (name == maxon::String(BACKENDS[i]))
			return DEVICETYPE(maxon::Int(DEVICETYPE::OPTIX) + i);
	}
	return DEVICETYPE::NONE;
}

maxon::Result<DeviceSettings> GetDeviceSettings()
{
	iferr_scope;
	const cinema::BaseContainer stored = ReadSettings();
	DeviceSettings settings;
	const maxon::Int32 type = stored.GetInt32(SETTING_DEVICE_TYPE, maxon::Int32(DEVICETYPE::AUTOMATIC));
	settings.type = type >= 0 && type <= maxon::Int32(DEVICETYPE::METAL) ? DEVICETYPE(type) : DEVICETYPE::AUTOMATIC;
	settings.found = stored.GetString(SETTING_DEVICES_FOUND);
	const cinema::BaseContainer list = stored.GetContainer(SETTING_DEVICES);
	for (maxon::Int32 i = 0; ; ++i)
	{
		const cinema::BaseContainer* const entry = list.GetContainerInstance(i);
		if (!entry)
			break;
		CyclesDevice device;
		device.id = entry->GetString(DEVICE_ID);
		device.name = entry->GetString(DEVICE_NAME);
		device.type = entry->GetString(DEVICE_TYPE);
		device.use = entry->GetBool(DEVICE_USE, true);
		settings.devices.Append(std::move(device)) iferr_return;
	}
	return settings;
}

static void StoreDevices(const DeviceSettings& settings)
{
	cinema::BaseContainer stored = ReadSettings();
	stored.SetInt32(SETTING_DEVICE_TYPE, maxon::Int32(settings.type));
	stored.SetString(SETTING_DEVICES_FOUND, settings.found);
	cinema::BaseContainer list;
	for (maxon::Int i = 0; i < settings.devices.GetCount(); ++i)
	{
		cinema::BaseContainer entry;
		entry.SetString(DEVICE_ID, settings.devices[i].id);
		entry.SetString(DEVICE_NAME, settings.devices[i].name);
		entry.SetString(DEVICE_TYPE, settings.devices[i].type);
		entry.SetBool(DEVICE_USE, settings.devices[i].use);
		list.SetContainer(maxon::Int32(i), entry);
	}
	stored.SetContainer(SETTING_DEVICES, list);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, stored, false);
}

void SetDeviceType(DEVICETYPE type)
{
	iferr (DeviceSettings settings = GetDeviceSettings())
		return;
	settings.type = type;
	StoreDevices(settings);
}

void SetDeviceUse(const maxon::String& id, maxon::Bool use)
{
	iferr (DeviceSettings settings = GetDeviceSettings())
		return;
	for (CyclesDevice& device : settings.devices)
	{
		if (device.id == id)
			device.use = use;
	}
	StoreDevices(settings);
}

DEVICETYPE ResolveDeviceType(const DeviceSettings& settings)
{
	if (settings.type != DEVICETYPE::AUTOMATIC)
		return settings.type;
	// The bridge's order (bridge/c4d_bridge/render/devices.py).
	for (const maxon::Char* backend : BACKENDS)
	{
		for (const CyclesDevice& device : settings.devices)
		{
			if (device.type == maxon::String(backend))
				return TypeOfName(device.type);
		}
	}
	return DEVICETYPE::NONE;
}

maxon::Result<maxon::BaseArray<CyclesDevice>> DevicesOfType(const DeviceSettings& settings, DEVICETYPE type)
{
	iferr_scope;
	maxon::BaseArray<CyclesDevice> shown;
	const DEVICETYPE resolved = type == DEVICETYPE::AUTOMATIC ? ResolveDeviceType(settings) : type;
	if (resolved == DEVICETYPE::NONE)
		return shown;
	const maxon::String backend = DeviceTypeName(resolved);
	for (const CyclesDevice& device : settings.devices)
	{
		if (device.type == backend)
		{
			shown.Append(device) iferr_return;
		}
	}
	// The CPU next to the GPUs, as in Blender's preferences, but only when the back end has one.
	if (!shown.IsEmpty())
	{
		for (const CyclesDevice& device : settings.devices)
		{
			if (device.type == "CPU"_s)
			{
				shown.Append(device) iferr_return;
			}
		}
	}
	return shown;
}

maxon::Result<maxon::String> FindDevices()
{
	iferr_scope;

	BridgeClient client;
	client.Open(BRIDGEMODE::FINAL) iferr_return;
	const maxon::DataDictionary hello = client.GetHello();
	client.Shutdown() iferr_ignore("Blender is ended with the client either way.");

	DeviceSettings settings = GetDeviceSettings() iferr_return;
	maxon::BaseArray<maxon::Data> listed;
	hello.GetCopy("cycles_devices"_s, listed) iferr_return;
	maxon::BaseArray<CyclesDevice> found;
	for (const maxon::Data& data : listed)
	{
		const maxon::Result<maxon::DataDictionary> entry = data.Get<maxon::DataDictionary>();
		if (entry != maxon::OK)
			continue;
		CyclesDevice device;
		device.id = GetJsonString(entry.GetValue(), "id"_s);
		device.name = GetJsonString(entry.GetValue(), "name"_s);
		device.type = GetJsonString(entry.GetValue(), "type"_s);
		if (device.id.IsEmpty())
			continue;
		// A device that is known keeps its tick; a new one is ticked unless it is the CPU (Blender's rule).
		device.use = device.type != "CPU"_s;
		for (const CyclesDevice& known : settings.devices)
		{
			if (known.id == device.id && known.type == device.type)
				device.use = known.use;
		}
		found.Append(std::move(device)) iferr_return;
	}
	settings.devices = std::move(found);
	settings.found = FormatString("@ device(s), Blender @.", settings.devices.GetCount(), GetJsonString(hello, "blender"_s));
	StoreDevices(settings);
	return settings.found;
}

void ResetDeviceSettings()
{
	cinema::BaseContainer stored = ReadSettings();
	stored.RemoveData(SETTING_DEVICE_TYPE);
	stored.RemoveData(SETTING_DEVICES);
	stored.RemoveData(SETTING_DEVICES_FOUND);
	cinema::SetWorldPluginData(ID_CYCLES_VIDEOPOST, stored, false);
}

maxon::String DescribeDeviceSettings()
{
	iferr (const DeviceSettings settings = GetDeviceSettings())
		return "{}"_s;
	JsonWriter json;
	json.BeginObject();
	json.Int("type", maxon::Int64(settings.type));
	json.String("found", settings.found);
	json.BeginArray("devices");
	for (const CyclesDevice& device : settings.devices)
	{
		json.BeginObject();
		json.String("id", device.id);
		json.String("name", device.name);
		json.String("type", device.type);
		json.Bool("use", device.use);
		json.EndObject();
	}
	json.EndArray();
	json.BeginArray("shown");
	iferr (const maxon::BaseArray<CyclesDevice> shown = DevicesOfType(settings, settings.type))
	{
	}
	else
	{
		for (const CyclesDevice& device : shown)
			json.String(nullptr, device.id);
	}
	json.EndArray();
	json.BeginObject("request");
	WriteDeviceSettings(json);
	json.EndObject();
	json.EndObject();
	return json.GetText();
}

void WriteDeviceSettings(JsonWriter& request)
{
	iferr (const DeviceSettings settings = GetDeviceSettings())
		return;
	if (settings.type != DEVICETYPE::AUTOMATIC)
		request.String("device_type", DeviceTypeName(settings.type));
	if (settings.devices.IsEmpty() || settings.type == DEVICETYPE::NONE)
		return;
	// The ticked devices of the type that is used; the bridge ticks exactly these.
	iferr (const maxon::BaseArray<CyclesDevice> shown = DevicesOfType(settings, settings.type))
		return;
	// A type this computer's list has no device of: the bridge says so and picks another.
	if (shown.IsEmpty())
		return;
	request.BeginArray("devices");
	for (const CyclesDevice& device : shown)
	{
		if (device.use)
			request.String(nullptr, device.id);
	}
	request.EndArray();
}

} // namespace cyclesrender
