// SPDX-License-Identifier: Apache-2.0
#ifndef TCYCLESCAMERA_H__
#define TCYCLESCAMERA_H__

// Parameter IDs of the Cycles Camera tag. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// The tag counts for the camera it sits on (not for cameras below it). Each tab starts with a
// switch; the tab's other settings count only while it is on (Depth of Field checked, a lens
// type other than "As the Camera's Projection"; the Color tab has none: at its defaults it changes
// nothing). Depth of field is the tag's alone: unchecked
// means none, as on Blender's camera.
// Values are Blender's own (scene/camera_export.cpp sends them as they are), defaults are
// Blender's.
enum
{
	// Tabs
	CYCLESCAMERA_GROUP_DOF = 2000,
	CYCLESCAMERA_GROUP_LENS = 2001,
	CYCLESCAMERA_GROUP_APERTURE = 2002,        // a group inside the Depth of Field tab
	CYCLESCAMERA_GROUP_PANORAMA = 2003,        // a group inside the Lens tab
	CYCLESCAMERA_GROUP_IMAGER = 2004,
	CYCLESCAMERA_GROUP_IMAGER_BUTTONS = 2005,  // the row of buttons below the list
	CYCLESCAMERA_GROUP_COLOR = 2006,           // (0.3.0) between the Lens and the Imager tab

	// Depth of field (Blender's camera, Depth of Field panel)
	// Checked: on, with this tab's values, whatever the render settings say. Unchecked (the
	// default, as Blender's): no depth of field for this camera, whatever its own values and the
	// render settings say. (Until the maintainer's decision of 2026-11-12 this id was a choice of
	// three; the tag was never released.)
	CYCLESCAMERA_DOF_ENABLE = 1000,
	CYCLESCAMERA_DOF_FOCUS_OBJECT = 1001,      // its distance along the camera's view axis replaces the focus distance
	CYCLESCAMERA_DOF_FOCUS_DISTANCE = 1002,    // document units
	CYCLESCAMERA_DOF_FSTOP = 1003,
	CYCLESCAMERA_DOF_BLADES = 1004,            // 0: round; fewer than 3 is round in Blender
	CYCLESCAMERA_DOF_ROTATION = 1005,          // radians
	CYCLESCAMERA_DOF_RATIO = 1006,             // anamorphic bokeh
	CYCLESCAMERA_DOF_TAKE_FOCUS = 1007,        // button (0.3.0, F11): the camera's own focus distance into 1002

	// Lens (Blender's camera, Lens panel)
	CYCLESCAMERA_LENS_TYPE = 1100,
		CYCLESCAMERA_LENS_TYPE_CAMERA = 0,       // the camera's own projection
		CYCLESCAMERA_LENS_TYPE_PERSPECTIVE = 1,  // the camera's focal length and sensor, whatever its projection
		CYCLESCAMERA_LENS_TYPE_ORTHOGRAPHIC = 2, // as wide as CYCLESCAMERA_LENS_ORTHO_SCALE
		CYCLESCAMERA_LENS_TYPE_PANORAMIC = 3,    // Cycles only
	CYCLESCAMERA_LENS_ORTHO_SCALE = 1101,      // width of the view, document units
	CYCLESCAMERA_LENS_PANORAMA_TYPE = 1102,
		CYCLESCAMERA_PANORAMA_EQUIRECTANGULAR = 0,
		CYCLESCAMERA_PANORAMA_EQUIANGULAR_CUBEMAP_FACE = 1,
		CYCLESCAMERA_PANORAMA_MIRRORBALL = 2,
		CYCLESCAMERA_PANORAMA_FISHEYE_EQUIDISTANT = 3,
		CYCLESCAMERA_PANORAMA_FISHEYE_EQUISOLID = 4,
		CYCLESCAMERA_PANORAMA_FISHEYE_LENS_POLYNOMIAL = 5,
		CYCLESCAMERA_PANORAMA_CENTRAL_CYLINDRICAL = 6,
	CYCLESCAMERA_LENS_FISHEYE_FOV = 1103,      // radians
	CYCLESCAMERA_LENS_FISHEYE_LENS = 1104,     // millimetres
	CYCLESCAMERA_LENS_LATITUDE_MIN = 1105,     // radians
	CYCLESCAMERA_LENS_LATITUDE_MAX = 1106,
	CYCLESCAMERA_LENS_LONGITUDE_MIN = 1107,
	CYCLESCAMERA_LENS_LONGITUDE_MAX = 1108,
	CYCLESCAMERA_LENS_POLYNOMIAL_K0 = 1109,
	CYCLESCAMERA_LENS_POLYNOMIAL_K1 = 1110,
	CYCLESCAMERA_LENS_POLYNOMIAL_K2 = 1111,
	CYCLESCAMERA_LENS_POLYNOMIAL_K3 = 1112,
	CYCLESCAMERA_LENS_POLYNOMIAL_K4 = 1113,
	CYCLESCAMERA_LENS_CYLINDER_U_MIN = 1114,   // radians
	CYCLESCAMERA_LENS_CYLINDER_U_MAX = 1115,
	CYCLESCAMERA_LENS_CYLINDER_V_MIN = 1116,   // heights on a cylinder of the radius below (Blender's numbers)
	CYCLESCAMERA_LENS_CYLINDER_V_MAX = 1117,
	CYCLESCAMERA_LENS_CYLINDER_RADIUS = 1118,

	// The Color tab (0.3.0, F13 W6): the camera's base colour correction, applied to the scene-linear
	// picture before the Imager's layers and before the view transform, whatever the Imager's switch
	// says. 1300 to 1399 are kept for it; corrections that come later (white balance, tone mapping,
	// saturation) go below exposure in the order they are applied, each as a group of its own.
	// Blender's compositor applies it (scene/imager_export.cpp: the tree's first node); a Redshift
	// camera's own exposure multiplies with it.
	CYCLESCAMERA_COLOR_EXPOSURE = 1300,        // stops: 1 doubles the picture; 0 (the default) changes nothing

	// The Imager tab: effects on the rendered picture, made by Blender's compositor, as a list of
	// layers like Cinema 4D's field lists: each layer is one effect with a name, a switch and an
	// opacity; the top layer is applied last; the selected layer's settings show below the list.
	CYCLESCAMERA_IMAGER_ENABLE = 1200,         // the tab's switch: off, the picture is the render
	// The list itself: a container that holds the layers' numbers under 0, 1, 2, ..., the top layer
	// first. A layer's number is given once and never again in the same tag (CYCLESCAMERA_IMAGER_NEXT).
	CYCLESCAMERA_IMAGER_LAYERS = 1201,
	CYCLESCAMERA_IMAGER_LIST = 1202,           // what the Attribute Manager shows of it (a view; kept in line with the layers)
	CYCLESCAMERA_IMAGER_SELECTED = 1203,       // the number of the layer whose settings show below the list
	CYCLESCAMERA_IMAGER_ADD = 1204,            // a menu: choosing an effect adds a layer of it; holds nothing
	CYCLESCAMERA_IMAGER_REMOVE = 1205,         // buttons: they act on the selected layer
	CYCLESCAMERA_IMAGER_UP = 1206,
	CYCLESCAMERA_IMAGER_DOWN = 1207,
	CYCLESCAMERA_IMAGER_NEXT = 1208,           // the number the next new layer gets

	// What a layer stores lies in the tag's own container, each field under
	//     IMAGERLAYER_BASE + layer * IMAGERLAYER_STRIDE + field
	// which is also the field's parameter id (for scripts, animation tracks and takes). Layers are
	// numbered from 1 to IMAGERLAYER_MAXIMUM. This formula and the fields below are stored in scene
	// files: never change them, and never give a retired field's number to another.
	IMAGERLAYER_BASE = 0x01000000,
	IMAGERLAYER_STRIDE = 0x00010000,
	IMAGERLAYER_MAXIMUM = 32000,
		IMAGERLAYER_GROUP = 0,                   // nothing stored: the group of the layer's settings in the Attribute Manager
		IMAGERLAYER_EFFECT = 1,                  // text: Blender's own name of the effect ("Vignette"); the layer is what this names
		IMAGERLAYER_SOURCE = 2,                  // where the effect comes from
			IMAGERLAYER_SOURCE_BLENDER = 0,        // a compositor node group of Blender's bundled asset library
			IMAGERLAYER_SOURCE_FILE = 1,           // reserved: a node group of a .blend file (custom presets), named by IMAGERLAYER_FILE
			IMAGERLAYER_SOURCE_NODE = 2,           // a plain compositor node, IMAGERLAYER_EFFECT its Blender type (0.3.0: "CompositorNodeExposure")
			IMAGERLAYER_SOURCE_BUILT = 3,          // (0.3.0, F13) an effect the plugin builds of several of Blender's nodes, IMAGERLAYER_EFFECT its name in the table ("Edges")
		IMAGERLAYER_NAME = 3,                    // text: the layer's name in the list
		IMAGERLAYER_ENABLED = 4,                 // the layer's switch
		IMAGERLAYER_OPACITY = 5,                 // 0 to 1: how much of the effect is mixed over what is below it
		IMAGERLAYER_FILE = 6,                    // reserved: the .blend file of IMAGERLAYER_SOURCE_FILE
		IMAGERLAYER_INFO = 7,                    // reserved: what a Blender said the group of a file takes (as the Cycles Proxy keeps its file's content)
		IMAGERLAYER_ENGINE = 8,                  // (0.3.0, F13) which of Blender's engines the layer applies in; a layer without it (saved before) applies in both
			IMAGERLAYER_ENGINE_BOTH = 0,
			IMAGERLAYER_ENGINE_CYCLES = 1,
			IMAGERLAYER_ENGINE_EEVEE = 2,
		IMAGERLAYER_PANELS = 100,                // nothing stored: 100 to 199 are the groups of an effect's panels in the Attribute Manager
		// From here on the effect's settings, each under the number the table gives it
		// (tags/imager_effects_table.h, generated: 1000 + n for Blender's socket "Socket_<n>", 10000 and
		// up for a socket of another name). A menu's value is a number made from the option's name
		// (tags/imager_effects.cpp, GetImagerOptionValue), so that an option Blender adds moves nothing.
		IMAGERLAYER_SETTINGS = 1000
};

#endif // TCYCLESCAMERA_H__
