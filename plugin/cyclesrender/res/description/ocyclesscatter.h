// SPDX-License-Identifier: Apache-2.0
#ifndef OCYCLESSCATTER_H__
#define OCYCLESSCATTER_H__

// Parameter IDs of the Cycles Scatter object. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// Blocks: 1000 the distribution, 1100 where copies may go (the restrictions, "Shader Distribution"), 1200 the
// copies varied by shaders, 1300 effectors and fields, 1400 the viewport's display, 2000 tabs and groups. A
// parameter keeps its number when it moves (F11). Where one value became a range or one axis three (F11 step 3),
// the old parameter is one of the new ones, with the meaning it had: a scene saved before renders the same copies.
enum
{
	// Tabs and groups
	CYCLESSCATTER_GROUP_DISTRIBUTION = 2000,
	CYCLESSCATTER_GROUP_EFFECTORS = 2001,
	CYCLESSCATTER_GROUP_DISPLAY = 2002,
	CYCLESSCATTER_GROUP_RESTRICT = 2003,
	CYCLESSCATTER_GROUP_VARY = 2004,
	CYCLESSCATTER_GROUP_VARY_POSITION = 2005,
	CYCLESSCATTER_GROUP_VARY_ROTATION = 2006,
	CYCLESSCATTER_GROUP_VARY_SCALE = 2007,
	CYCLESSCATTER_GROUP_EFFECTOR_LIST = 2008,
	CYCLESSCATTER_GROUP_FIELD_LIST = 2009,

	// Distribution
	CYCLESSCATTER_MODE = 1000,
		CYCLESSCATTER_MODE_VERTICES = 0,         // one copy on every point of the surface
		CYCLESSCATTER_MODE_SURFACE = 1,          // Count copies over the surface, by area
		CYCLESSCATTER_MODE_POISSON = 2,          // copies at least Radius apart until the surface is full
		CYCLESSCATTER_MODE_FILE = 3,             // one copy per line of a CSV file
		CYCLESSCATTER_MODE_VOLUME = 4,           // Count copies inside the surface's closed mesh, by volume
	CYCLESSCATTER_SURFACE = 1001,                // the object scattered over (its polygons, generated or not)
	CYCLESSCATTER_COUNT = 1002,                  // Surface, Volume: the copies made (fewer only with CYCLESSCATTER_REMOVE_COPIES, or without room); Poisson with CYCLESSCATTER_POISSON_LIMIT: the most made
	CYCLESSCATTER_SEED = 1003,
	CYCLESSCATTER_POISSON_RADIUS = 1004,         // document units
	CYCLESSCATTER_POISSON_SAMPLES = 1005,        // the surface counts as full after this many tries in a row fail
	CYCLESSCATTER_FILE = 1006,                   // CSV: x, y, z [, rotation h, p, b in degrees [, scale or sx, sy, sz]] per line
	CYCLESSCATTER_KEEP_AWAY = 1007,              // the spacing radius: no copy within it of another; document units, 0: none
	CYCLESSCATTER_NORMAL_ALIGN = 1008,           // 0..1: from the up vector (0) to the surface's normal (1)
	CYCLESSCATTER_UP_VECTOR = 1009,              // in the scatter object's axes
	CYCLESSCATTER_CHILD_ORDER = 1010,            // which child a copy shows
		CYCLESSCATTER_CHILD_ORDER_RANDOM = 0,
		CYCLESSCATTER_CHILD_ORDER_ITERATE = 1,
	CYCLESSCATTER_REMOVE_COPIES = 1011,          // Surface: the restrictions leave copies out instead of moving them elsewhere
	CYCLESSCATTER_FILE_SEQUENCE = 1012,          // the CSV file is one of a sequence: the file of each frame is read
	CYCLESSCATTER_FILE_FRAME_OFFSET = 1013,      // added to the document's frame to find the file
	CYCLESSCATTER_POISSON_LIMIT = 1014,          // Poisson stops at CYCLESSCATTER_COUNT copies (off, and for scenes saved before 0.3.0: until full)

	// Where copies may go
	CYCLESSCATTER_VERTEX_MAP = 1100,             // a vertex map tag of the surface
	CYCLESSCATTER_VERTEX_MAP_LIMIT = 1101,       // 0..1: no copy where the map is below it
	CYCLESSCATTER_DENSITY_SHADER = 1102,         // lighter: more copies
	CYCLESSCATTER_DENSITY_MIN = 1103,            // 0..1: the share of copies kept where the shader is black
	CYCLESSCATTER_DENSITY_MAX = 1104,            // 0..1: the share kept where it is white
	CYCLESSCATTER_NORMAL_THRESHOLD = 1105,       // degrees: copies only where the normal is this close to the up vector

	// The copies varied by shaders (sampled where each copy sits)
	// Each value goes from its Min at black to its Max at white (one sample of the shader for all of them).
	CYCLESSCATTER_POSITION_SHADER = 1200,
	CYCLESSCATTER_POSITION_DISTANCE = 1201,      // document units along the copy's Y (after its rotation): the Max (the Min is CYCLESSCATTER_POSITION_Y_MIN)
	CYCLESSCATTER_SCALE_SHADER = 1202,
	CYCLESSCATTER_SCALE_MIN = 1203,              // uniform: the scale at black
	CYCLESSCATTER_SCALE_MAX = 1204,              // uniform: the scale at white
	CYCLESSCATTER_ROTATION_SHADER = 1205,
	CYCLESSCATTER_ROTATION_ANGLE = 1206,         // radians: the pitch (about the copy's X) at white, the Max (the Min is CYCLESSCATTER_ROTATION_P_MIN)
	CYCLESSCATTER_SCALE_UNIFORM = 1207,          // on (default, and before 0.3.0): one range for the three axes (1203, 1204); off: per axis (1208, 1209)
	CYCLESSCATTER_SCALE_MIN_AXES = 1208,         // vector, per axis of the copy: the scale at black
	CYCLESSCATTER_SCALE_MAX_AXES = 1209,         // vector: the scale at white
	CYCLESSCATTER_ROTATION_H_MIN = 1210,         // radians: the heading (about the copy's Y), turned first
	CYCLESSCATTER_ROTATION_H_MAX = 1211,
	CYCLESSCATTER_ROTATION_P_MIN = 1212,         // radians: the pitch (about the copy's X), turned second; its Max is 1206
	CYCLESSCATTER_ROTATION_B_MIN = 1213,         // radians: the bank (about the copy's Z), turned last
	CYCLESSCATTER_ROTATION_B_MAX = 1214,
	CYCLESSCATTER_POSITION_Y_MIN = 1215,         // document units along the copy's Y: the Min (its Max is 1201)
	CYCLESSCATTER_POSITION_U_MIN = 1216,         // document units across the surface, along its U (the tangent from the UVs, else the frame of the normal)
	CYCLESSCATTER_POSITION_U_MAX = 1217,
	CYCLESSCATTER_POSITION_V_MIN = 1218,         // document units across the surface, along its V
	CYCLESSCATTER_POSITION_V_MAX = 1219,

	// Effectors and fields
	CYCLESSCATTER_EFFECTORS = 1300,              // MoGraph effectors, applied in the list's order
	CYCLESSCATTER_FIELDS = 1301,                 // a field list of the object itself, sampled at each copy
	CYCLESSCATTER_FIELDS_AFFECT = 1302,          // what the field list weights
		CYCLESSCATTER_FIELDS_AFFECT_DENSITY = 0,
		CYCLESSCATTER_FIELDS_AFFECT_SCALE = 1,
		CYCLESSCATTER_FIELDS_AFFECT_BOTH = 2,

	// The viewport
	CYCLESSCATTER_DISPLAY = 1400,
		CYCLESSCATTER_DISPLAY_NONE = 0,
		CYCLESSCATTER_DISPLAY_POINTS = 1,
		CYCLESSCATTER_DISPLAY_BOXES = 2,
	CYCLESSCATTER_DISPLAY_RATE = 1401,           // 0..1: the share of copies drawn
	CYCLESSCATTER_DISPLAY_COLOR_MODE = 1402,
		CYCLESSCATTER_DISPLAY_COLOR_COPY = 0,      // each copy's MoGraph colour
		CYCLESSCATTER_DISPLAY_COLOR_CUSTOM = 1,
	CYCLESSCATTER_DISPLAY_COLOR = 1403
};

#endif // OCYCLESSCATTER_H__
