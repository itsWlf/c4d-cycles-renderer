// SPDX-License-Identifier: Apache-2.0
// Blender's Cycles render devices in the plugin's preferences: which back end (OptiX, CUDA, HIP,
// oneAPI, Metal) and which devices the Render Settings' "GPU" means, as in Blender's own
// Preferences > System > Cycles Render Devices.

#ifndef CYCLESRENDER_DEVICES_H__
#define CYCLESRENDER_DEVICES_H__

#include "c4d.h"

#include "../session/json_writer.h"

namespace cyclesrender
{

/// The device types of the Preferences page, as stored (prefscyclesrender.h has the same numbers).
/// Never renumber: the choice is stored in Cinema 4D's preferences.
enum class DEVICETYPE
{
	AUTOMATIC = 0,	///< the first back end that lists a device: OptiX, CUDA, HIP, oneAPI, Metal
	NONE = 1,				///< no GPU: "GPU" renders on the CPU
	OPTIX = 2,
	CUDA = 3,
	HIP = 4,
	ONEAPI = 5,
	METAL = 6
} MAXON_ENUM_LIST(DEVICETYPE);

/// One render device as a Blender reported it (its hello reply, `cycles_devices`).
struct CyclesDevice
{
	maxon::String id;       ///< Blender's persistent id of the device
	maxon::String name;
	maxon::String type;     ///< CPU, OPTIX, CUDA, HIP, ONEAPI or METAL
	maxon::Bool use = true;	///< ticked on the Preferences page
};

/// What the Preferences page holds. May be read from any thread.
struct DeviceSettings
{
	DEVICETYPE type = DEVICETYPE::AUTOMATIC;
	maxon::BaseArray<CyclesDevice> devices;   ///< empty until a Blender was asked (FindDevices)
	maxon::String found;                      ///< when and by which Blender the list was made
};

/// Reads the stored settings.
maxon::Result<DeviceSettings> GetDeviceSettings();

/// Stores the device type. Main thread only, as every write to the preferences.
void SetDeviceType(DEVICETYPE type);

/// Ticks or unticks the device with Blender's id @p id. Main thread only.
void SetDeviceUse(const maxon::String& id, maxon::Bool use);

/// Blender's name of a back end ("OPTIX", ...), "NONE", or empty for AUTOMATIC.
maxon::String DeviceTypeName(DEVICETYPE type);

/// The back end AUTOMATIC stands for with the stored list (the first that lists a device), or
/// NONE when the list has no GPU.
DEVICETYPE ResolveDeviceType(const DeviceSettings& settings);

/// The devices of the Preferences page's list for a device type: those of the back end, and the
/// CPU after them, as Blender's preferences show them. None for NONE.
maxon::Result<maxon::BaseArray<CyclesDevice>> DevicesOfType(const DeviceSettings& settings, DEVICETYPE type);

//----------------------------------------------------------------------------------------
/// Starts a Blender in the background (about 2 s), takes the devices its hello reply lists, and
/// stores them, keeping the ticks of devices already known (a new GPU is ticked, a new CPU not,
/// as Blender does). Blocks: call it from the Preferences page's button. Nothing of the user's
/// own Blender set-up is read or written: Blender runs with --factory-startup
/// (session/blender_process.cpp), which neither loads nor saves its preferences.
/// @return  The new list's description for the page.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::String> FindDevices();

/// Forgets the device type and the list (AUTOMATIC, nothing found): for tests, which must not
/// leave settings behind in a preferences folder. Main thread only.
void ResetDeviceSettings();

/// The stored settings as JSON, for tests: {"type", "found", "devices": [{"id", "name", "type",
/// "use"}], "shown": [ids of the page's checkboxes], "request": {the fields WriteDeviceSettings writes}}.
maxon::String DescribeDeviceSettings();

/// Writes the fields `device_type` and `devices` of a `settings` request (protocol/protocol.md),
/// from the stored settings: nothing for AUTOMATIC without a list (the bridge then picks as it
/// always did).
void WriteDeviceSettings(JsonWriter& request);

} // namespace cyclesrender

#endif // CYCLESRENDER_DEVICES_H__
