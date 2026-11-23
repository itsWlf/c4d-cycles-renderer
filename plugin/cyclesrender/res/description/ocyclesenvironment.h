// SPDX-License-Identifier: Apache-2.0
#ifndef OCYCLESENVIRONMENT_H__
#define OCYCLESENVIRONMENT_H__

// Parameter IDs of the Cycles Environment object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Blocks: 1000 the type, strength and roles, 1100 Color, 1200 Sky Texture, 1300 HDRI, 1400 World Fog,
// 2000 tabs and groups.
// The Sky Texture's and the HDRI's settings are Blender's own: their names, options and defaults come
// from the generated node table at run time (scene/environment_export.h, GetEnvironmentNodeSettings),
// in the order of Blender's nodes. An option is stored as its position in the table's list of that
// setting's options. The layout is to be redesigned (internal roadmap, F11): a parameter keeps its number.
enum
{
	// Tabs and groups
	CYCLESENV_GROUP_ENVIRONMENT = 2000,
	CYCLESENV_GROUP_SKY = 2001,
	CYCLESENV_GROUP_HDRI = 2002,
	CYCLESENV_GROUP_FOG = 2003,                  // the World Fog tab

	CYCLESENV_TYPE = 1000,
		CYCLESENV_TYPE_COLOR = 0,
		CYCLESENV_TYPE_SKY = 1,
		CYCLESENV_TYPE_HDRI = 2,
		CYCLESENV_TYPE_MATERIAL = 3,               // the Cycles world material on the object's texture tag
	CYCLESENV_STRENGTH = 1001,                   // the Background node's strength, for every type
	CYCLESENV_SEEN_BY_CAMERA = 1002,             // the highest one that is, is what the camera sees
	CYCLESENV_LIGHTS_SCENE = 1003,               // the highest one that does, lights the scene

	// Color
	CYCLESENV_COLOR = 1100,

	// Sky Texture (Blender's ShaderNodeTexSky, its settings in the node's order)
	CYCLESENV_SKY_TYPE = 1200,
	CYCLESENV_SKY_SUN_DISC = 1201,
	CYCLESENV_SKY_SUN_SIZE = 1202,               // radians
	CYCLESENV_SKY_SUN_INTENSITY = 1203,
	CYCLESENV_SKY_SUN_ELEVATION = 1204,          // radians, in the object's axes
	CYCLESENV_SKY_SUN_ROTATION = 1205,           // radians, in the object's axes
	CYCLESENV_SKY_ALTITUDE = 1206,               // metres, as Blender's (not document units)
	CYCLESENV_SKY_AIR_DENSITY = 1207,
	CYCLESENV_SKY_AEROSOL_DENSITY = 1208,
	CYCLESENV_SKY_OZONE_DENSITY = 1209,
	CYCLESENV_SKY_TURBIDITY = 1210,
	CYCLESENV_SKY_GROUND_ALBEDO = 1211,
	CYCLESENV_SKY_SUN_LIGHT = 1212,              // a light whose direction places the sun (in the world)

	// HDRI (Blender's ShaderNodeTexEnvironment)
	CYCLESENV_HDRI_FILE = 1300,
	CYCLESENV_HDRI_COLORSPACE = 1301,
	CYCLESENV_HDRI_PROJECTION = 1302,
	CYCLESENV_HDRI_INTERPOLATION = 1303,
	CYCLESENV_HDRI_MAKE_RELATIVE = 1304,         // buttons, as the image nodes' (nodespace/path_commands.h)
	CYCLESENV_HDRI_MAKE_ABSOLUTE = 1305,

	// World Fog (a Principled Volume on the World Output's Volume, with the Cycles Fog's wording)
	CYCLESENV_FOG_ENABLE = 1400,
	CYCLESENV_FOG_DENSITY = 1401,                // per metre
	CYCLESENV_FOG_COLOR = 1402,
	CYCLESENV_FOG_ANISOTROPY = 1403,
	CYCLESENV_FOG_ABSORPTION_COLOR = 1404,
	CYCLESENV_FOG_EMISSION_STRENGTH = 1405,
	CYCLESENV_FOG_EMISSION_COLOR = 1406
};

#endif // OCYCLESENVIRONMENT_H__
