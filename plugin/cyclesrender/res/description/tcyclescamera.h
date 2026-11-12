#ifndef TCYCLESCAMERA_H__
#define TCYCLESCAMERA_H__

// Parameter IDs of the Cycles Camera tag. They are stored in scene files: never renumber, and
// never reuse a retired number.
//
// The tag counts for the camera it sits on (not for cameras below it). Each tab starts with a
// switch; the tab's other settings count only while it is on (Depth of Field checked, a lens
// type other than "As the Camera's Projection"). Depth of field is the tag's alone: unchecked
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
	CYCLESCAMERA_LENS_CYLINDER_RADIUS = 1118
};

#endif // TCYCLESCAMERA_H__
