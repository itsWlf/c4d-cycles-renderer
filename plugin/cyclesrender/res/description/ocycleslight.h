#ifndef OCYCLESLIGHT_H__
#define OCYCLESLIGHT_H__

// Parameter IDs of the Cycles Light object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// The values are Blender's own (scene/light_export.cpp sends them as they are), the defaults are
// those of a light Blender 5.2.3 adds (Add > Light), lengths in document units. The Cycles Light
// tag (tcycleslight.h) uses the same numbers for the same settings, so that one reader serves
// both (lights/light_settings.h); keep the two headers equal where they overlap.
enum
{
	// Tabs and groups
	CYCLESLIGHT_GROUP_LIGHT = 2000,
	CYCLESLIGHT_GROUP_BEAM = 2001,               // inside the Light tab: Blender's "Beam Shape"
	CYCLESLIGHT_GROUP_SETTINGS = 2002,           // Cycles' "Settings" panel
	CYCLESLIGHT_GROUP_VISIBILITY = 2003,         // the object's "Ray Visibility"
	CYCLESLIGHT_GROUP_EEVEE = 2004,
	CYCLESLIGHT_GROUP_EEVEE_SHADOW = 2005,       // inside the Eevee tab
	CYCLESLIGHT_GROUP_EEVEE_INFLUENCE = 2006,
	CYCLESLIGHT_GROUP_EEVEE_DISTANCE = 2007,
	CYCLESLIGHT_GROUP_LINKING = 2008,            // the object only: which objects it lights

	// Light (Blender's Light panel)
	CYCLESLIGHT_TYPE = 1000,
		CYCLESLIGHT_TYPE_POINT = 0,
		CYCLESLIGHT_TYPE_SUN = 1,
		CYCLESLIGHT_TYPE_SPOT = 2,
		CYCLESLIGHT_TYPE_AREA = 3,
	CYCLESLIGHT_USE_TEMPERATURE = 1001,
	CYCLESLIGHT_TEMPERATURE = 1002,              // kelvin
	CYCLESLIGHT_COLOR = 1003,                    // "Tint" while the temperature is used
	CYCLESLIGHT_ENERGY = 1004,                   // watts; for a sun W/m2 ("Strength")
	CYCLESLIGHT_EXPOSURE = 1005,                 // stops: the power is energy * 2^exposure
	CYCLESLIGHT_NORMALIZE = 1006,
	CYCLESLIGHT_RADIUS = 1007,                   // point and spot (Blender's shadow_soft_size); document units
	CYCLESLIGHT_SOFT_FALLOFF = 1008,             // point and spot
	CYCLESLIGHT_ANGLE = 1009,                    // sun; radians
	CYCLESLIGHT_SHAPE = 1010,                    // area
		CYCLESLIGHT_SHAPE_SQUARE = 0,
		CYCLESLIGHT_SHAPE_RECTANGLE = 1,
		CYCLESLIGHT_SHAPE_DISK = 2,
		CYCLESLIGHT_SHAPE_ELLIPSE = 3,
	CYCLESLIGHT_SIZE = 1011,                     // area; document units ("Size X" for a rectangle or ellipse)
	CYCLESLIGHT_SIZE_Y = 1012,
	CYCLESLIGHT_SPOT_SIZE = 1013,                // spot; the full cone angle, radians
	CYCLESLIGHT_SPOT_BLEND = 1014,
	CYCLESLIGHT_SHOW_CONE = 1015,                // spot; the viewport only
	CYCLESLIGHT_SPREAD = 1016,                   // area; radians

	// Settings (Cycles)
	CYCLESLIGHT_MAX_BOUNCES = 1100,
	CYCLESLIGHT_CAST_SHADOW = 1101,
	CYCLESLIGHT_MULTIPLE_IMPORTANCE = 1102,
	CYCLESLIGHT_SHADOW_CAUSTICS = 1103,
	CYCLESLIGHT_PORTAL = 1104,                   // area

	// Ray visibility
	CYCLESLIGHT_VISIBLE_CAMERA = 1200,
	CYCLESLIGHT_VISIBLE_DIFFUSE = 1201,
	CYCLESLIGHT_VISIBLE_GLOSSY = 1202,
	CYCLESLIGHT_VISIBLE_TRANSMISSION = 1203,
	CYCLESLIGHT_VISIBLE_VOLUME_SCATTER = 1204,

	// Eevee
	CYCLESLIGHT_EEVEE_DIFFUSE_FACTOR = 1300,
	CYCLESLIGHT_EEVEE_SPECULAR_FACTOR = 1301,
	CYCLESLIGHT_EEVEE_TRANSMISSION_FACTOR = 1302,
	CYCLESLIGHT_EEVEE_VOLUME_FACTOR = 1303,
	CYCLESLIGHT_EEVEE_USE_CUSTOM_DISTANCE = 1304,
	CYCLESLIGHT_EEVEE_CUSTOM_DISTANCE = 1305,    // document units; not for a sun
	CYCLESLIGHT_EEVEE_SHADOW_JITTER = 1306,
	CYCLESLIGHT_EEVEE_SHADOW_OVERBLUR = 1307,    // percent
	CYCLESLIGHT_EEVEE_SHADOW_FILTER = 1308,
	CYCLESLIGHT_EEVEE_SHADOW_RESOLUTION = 1309,  // the smallest shadow map texel; document units
	CYCLESLIGHT_EEVEE_SHADOW_ABSOLUTE = 1310,    // not for a sun

	// Light linking (the object only; the tag's light has Cinema 4D's own lists)
	CYCLESLIGHT_LINKING_MODE = 1400,
		CYCLESLIGHT_LINKING_MODE_EXCLUDE = 0,
		CYCLESLIGHT_LINKING_MODE_INCLUDE = 1,
	CYCLESLIGHT_LINKING_LIST = 1401
};

#endif // OCYCLESLIGHT_H__
