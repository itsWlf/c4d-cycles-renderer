#ifndef TCYCLESOBJECT_H__
#define TCYCLESOBJECT_H__

// Parameter IDs of the Cycles Object tag. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Each tab has an "enable" checkbox; a tab that is not enabled changes nothing. An enabled tab
// counts for the tag's object and everything below it, until another Cycles Object tag with
// that tab enabled.
enum
{
	// Tabs
	CYCLESOBJECT_GROUP_VOLUME = 2000,
	CYCLESOBJECT_GROUP_VISIBILITY = 2001,
	CYCLESOBJECT_GROUP_LINKING = 2002,
	CYCLESOBJECT_GROUP_INSTANCES = 2003,

	// Volume shading (objects that produce volume grids)
	CYCLESOBJECT_VOLUME_DENSITY = 1000,
	CYCLESOBJECT_VOLUME_COLOR = 1001,
	CYCLESOBJECT_VOLUME_ANISOTROPY = 1002,
	CYCLESOBJECT_VOLUME_EMISSION_STRENGTH = 1003,
	CYCLESOBJECT_VOLUME_EMISSION_COLOR = 1004,
	CYCLESOBJECT_VOLUME_BLACKBODY = 1005,
	// 1006 was a constant temperature; retired before release
	CYCLESOBJECT_VOLUME_DENSITY_GRID = 1007,
	CYCLESOBJECT_VOLUME_TEMPERATURE_GRID = 1008,
	CYCLESOBJECT_VOLUME_COLOR_GRID = 1009,
	CYCLESOBJECT_VOLUME_ENABLE = 1010,
	CYCLESOBJECT_VOLUME_TEMPERATURE = 1011,         // kelvin for a value of 1 in the fire grid
	CYCLESOBJECT_VOLUME_FIRE_MODE = 1012,           // what the fire ("temperature") grid holds
		CYCLESOBJECT_VOLUME_FIRE_MODE_AUTO = 0,       // a grid called "temperature" is one, any other is a brightness
		CYCLESOBJECT_VOLUME_FIRE_MODE_TEMPERATURE = 1,
		CYCLESOBJECT_VOLUME_FIRE_MODE_BRIGHTNESS = 2,

	// Visibility: which rays see the object (Blender's Object > Visibility panel)
	CYCLESOBJECT_VISIBILITY_ENABLE = 1100,
	CYCLESOBJECT_VISIBILITY_CAMERA = 1101,          // off = Blender's "indirect only"
	CYCLESOBJECT_VISIBILITY_DIFFUSE = 1102,
	CYCLESOBJECT_VISIBILITY_GLOSSY = 1103,
	CYCLESOBJECT_VISIBILITY_TRANSMISSION = 1104,
	CYCLESOBJECT_VISIBILITY_VOLUME_SCATTER = 1105,
	CYCLESOBJECT_VISIBILITY_SHADOW = 1106,
	CYCLESOBJECT_VISIBILITY_HOLDOUT = 1107,
	CYCLESOBJECT_VISIBILITY_SHADOW_CATCHER = 1108,

	// Light linking, from the object's side
	CYCLESOBJECT_LINKING_ENABLE = 1200,
	CYCLESOBJECT_LINKING_MODE = 1201,
		CYCLESOBJECT_LINKING_MODE_EXCLUDE = 0,        // the listed lights do not light the object
		CYCLESOBJECT_LINKING_MODE_INCLUDE = 1,        // only the listed lights do
	CYCLESOBJECT_LINKING_LIGHTS = 1202,

	// Instances on the particles of a particle group or the matrices of a Matrix object
	CYCLESOBJECT_INSTANCES_ENABLE = 1300,
	CYCLESOBJECT_INSTANCES_OBJECTS = 1301,
	CYCLESOBJECT_INSTANCES_SCALE = 1302,
	CYCLESOBJECT_INSTANCES_USE_RADIUS = 1303        // particles: scale each copy by its particle's radius
};

#endif // TCYCLESOBJECT_H__
