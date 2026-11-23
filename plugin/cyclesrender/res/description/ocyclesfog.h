// SPDX-License-Identifier: Apache-2.0
#ifndef OCYCLESFOG_H__
#define OCYCLESFOG_H__

// Parameter IDs of the Cycles Fog object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Blocks: 1000 the shape, 1100 the volume shading (Blender's Principled Volume, with its names and,
// but for the density, its defaults: objects/fog_object.cpp), 1200 the noise that varies the density
// (Cinema 4D's noises, sampled by the plugin: scene/fog_export.h), 2000 tabs and groups. The layout is
// to be redesigned (internal roadmap, F11): a parameter keeps its number when it moves.
enum
{
	// Tabs and groups
	CYCLESFOG_GROUP_FOG = 2000,
	CYCLESFOG_GROUP_NOISE = 2001,

	// Shape
	CYCLESFOG_SIZE = 1000,                       // the box's edges along X, Y and Z; document units

	// Volume shading (Principled Volume)
	CYCLESFOG_DENSITY = 1100,                    // per metre of the scene, as Blender's
	CYCLESFOG_COLOR = 1101,                      // the scattering colour
	CYCLESFOG_ANISOTROPY = 1102,
	CYCLESFOG_ABSORPTION_COLOR = 1103,
	CYCLESFOG_EMISSION_STRENGTH = 1104,
	CYCLESFOG_EMISSION_COLOR = 1105,
	CYCLESFOG_BLACKBODY_INTENSITY = 1106,
	CYCLESFOG_BLACKBODY_TINT = 1107,
	CYCLESFOG_TEMPERATURE = 1108,                // kelvin, of the whole box

	// Noise: the density times a noise of Cinema 4D's (C4DNoise), sampled into a grid
	CYCLESFOG_NOISE_ENABLE = 1200,
	CYCLESFOG_NOISE_TYPE = 1201,                 // a NoiseType of lib_noise.h, as the Noise shader's
	CYCLESFOG_NOISE_SPACE = 1202,
		CYCLESFOG_NOISE_SPACE_OBJECT = 0,          // moves, turns and scales with the box
		CYCLESFOG_NOISE_SPACE_WORLD = 1,           // stands still while the box moves through it
	CYCLESFOG_NOISE_SCALE = 1203,                // the size of the noise's features; document units
	CYCLESFOG_NOISE_RELATIVE_SCALE = 1204,       // per axis, times the scale
	CYCLESFOG_NOISE_OCTAVES = 1205,
	CYCLESFOG_NOISE_SEED = 1206,
	CYCLESFOG_NOISE_ABSOLUTE = 1207,
	CYCLESFOG_NOISE_LOW_CLIP = 1208,             // 0..1: noise at or below it is no fog
	CYCLESFOG_NOISE_HIGH_CLIP = 1209,            // 0..1: noise at or above it is the full density
	CYCLESFOG_NOISE_BRIGHTNESS = 1210,           // -1..1, added after the clip
	CYCLESFOG_NOISE_CONTRAST = 1211,             // -1..1, around 0.5 after the clip
	CYCLESFOG_NOISE_SPEED = 1212,                // the noise's time per second of the document (animation)
	CYCLESFOG_NOISE_RESOLUTION = 1213            // cells of the grid along the box's longest edge
};

#endif // OCYCLESFOG_H__
