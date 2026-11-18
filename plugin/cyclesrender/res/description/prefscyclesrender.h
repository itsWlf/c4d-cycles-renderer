#ifndef PREFSCYCLESRENDER_H__
#define PREFSCYCLESRENDER_H__

// Parameter IDs of the plugin's Preferences page. The page stores nothing itself: prefs/settings.cpp
// and prefs/devices.cpp keep the values under keys of their own.
enum
{
	PREF_CYCLES_MAIN_GROUP = 999,
	PREF_CYCLES_BLENDER_PATH = 1000,
	PREF_CYCLES_BLENDER_STATUS = 1001,
	PREF_CYCLES_DETECT = 1002,

	// Settings made new (0.2.0, F21): prefs/settings.h, GetDefaultEngine.
	PREF_CYCLES_NEW_GROUP = 996,
	PREF_CYCLES_DEFAULT_ENGINE = 1009,
		PREF_CYCLES_DEFAULT_ENGINE_CYCLES = 0,   // the same numbers as CYCLES_ENGINE_CYCLES and _EEVEE (vpcyclesrender.h)
		PREF_CYCLES_DEFAULT_ENGINE_EEVEE = 1,

	// Cycles Render Devices (as in Blender's Preferences > System)
	PREF_CYCLES_DEVICE_GROUP = 998,
	PREF_CYCLES_DEVICE_NOTE = 1006,
	PREF_CYCLES_DEVICE_TYPE = 1003,
		PREF_CYCLES_DEVICE_TYPE_AUTOMATIC = 0,   // the same numbers as prefs/devices.h, DEVICETYPE
		PREF_CYCLES_DEVICE_TYPE_NONE = 1,
		PREF_CYCLES_DEVICE_TYPE_OPTIX = 2,
		PREF_CYCLES_DEVICE_TYPE_CUDA = 3,
		PREF_CYCLES_DEVICE_TYPE_HIP = 4,
		PREF_CYCLES_DEVICE_TYPE_ONEAPI = 5,
		PREF_CYCLES_DEVICE_TYPE_METAL = 6,
	PREF_CYCLES_DEVICES_STATUS = 1004,
	PREF_CYCLES_FIND_DEVICES = 1005,
	// One checkbox per device of the chosen type, made by the page (prefs_page.cpp): 1100 and on.
	PREF_CYCLES_DEVICE_FIRST = 1100,
	PREF_CYCLES_DEVICE_LAST = 1163,

	// The IPR: switches for its riskier features, to turn them off when hunting a fault, and for
	// following changes while they are made (prefs/settings.h).
	PREF_CYCLES_IPR_GROUP = 997,
	PREF_CYCLES_IPR_SKIP_UNCHANGED = 1007,
	PREF_CYCLES_IPR_NAVIGATE = 1008,
	PREF_CYCLES_IPR_LIVE_UPDATES = 1010,	// 0.2.0, F22 (1009 is F21's Default Engine)
	PREF_CYCLES_IPR_THREADS_FREE = 1011,	// 0.2.0, B11
	PREF_CYCLES_IPR_OPTIMIZED_UPDATES = 1012	// 0.2.0, B11 (1013 to 1099 have never been used)
};

#endif // PREFSCYCLESRENDER_H__
