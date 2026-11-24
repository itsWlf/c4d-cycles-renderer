// SPDX-License-Identifier: Apache-2.0
#ifndef OCYCLESFOG_H__
#define OCYCLESFOG_H__

// Parameter IDs of the Cycles Fog object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Blocks: 1000 the shape, 1100 the volume shading (Blender's Principled Volume, with its names and,
// but for the density, its defaults: objects/fog_object.cpp), 1200 the noise that varies the density
// (Cinema 4D's noises, sampled by the plugin: scene/fog_export.h) and the grid's resolution, 1300 the
// texture that varies it too (any shader, sampled into the same grid), 2000 tabs and groups. A
// parameter keeps its number when it moves (F11 step 2 moved the resolution to the Fog tab).
enum
{
	// Tabs and groups
	CYCLESFOG_GROUP_FOG = 2000,
	CYCLESFOG_GROUP_NOISE = 2001,
	CYCLESFOG_GROUP_DENSITY = 2002,              // the Fog tab's section of what varies the density

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
	CYCLESFOG_NOISE_RESOLUTION = 1213,           // cells of the grid along the box's longest edge, for the noise
	                                             // and the texture ("Voxel Resolution", in the Fog tab since F11 step 2)

	// Texture: the density times a shader's value (any of Cinema 4D's shaders, owned by the fog), sampled
	// into the same grid as the noise
	CYCLESFOG_TEXTURE = 1300,                    // SHADERLINK; a shader linked turns the grid on
	CYCLESFOG_TEXTURE_PROJECTION = 1301,         // which of the box's axes the shader's U and V lie along
		CYCLESFOG_TEXTURE_PROJECTION_FRONT = 0,    // U along X, V down Y (W along Z): a picture upright seen from the front
		CYCLESFOG_TEXTURE_PROJECTION_RIGHT = 1,    // U along Z, V down Y (W along X): seen from the right
		CYCLESFOG_TEXTURE_PROJECTION_TOP = 2,      // U along X, V down Z (W along Y): seen from above
	CYCLESFOG_GRID_INFO = 1302                   // STATICTEXT: the grid's cells (stores nothing; the number is taken)
};

#endif // OCYCLESFOG_H__
