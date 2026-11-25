// SPDX-License-Identifier: Apache-2.0
// Ids of Redshift's own types and parameters that the exporter reads and that the SDK's headers do not
// list, or not in every supported SDK, or under a name that says less: one home for them. They are
// Redshift's, not this plugin's: never change one. Each says where it was found. The Redshift ids every
// SDK lists are used from its headers (orslight.h, drsfile.h).

#ifndef CYCLESRENDER_REDSHIFT_IDS_H__
#define CYCLESRENDER_REDSHIFT_IDS_H__

#include "c4d.h"

namespace cyclesrender
{

/// Redshift's file type, the data type of a light's texture parameters (the dome light's map, a
/// light's colour texture). Such a parameter is read with both levels of its id typed: this type, then
/// REDSHIFT_FILE_PATH (drsfile.h) as a string. The SDKs from 2026 on list it as CUSTOMDATATYPE_RSFILE
/// (ge_prepass.h); found by reading the dome light's description.
static const maxon::Int32 REDSHIFT_FILE_TYPE = 1036765;

/// Redshift's renderer: its video post in a document's render data, which holds Redshift's own render
/// settings (the SDK's VPrsrenderer, c4d_videopostdata.h).
static const maxon::Int32 REDSHIFT_RENDERER = 1036219;
/// Its render setting "Image Units Scale", from Redshift 2026 on, 100 by default (Redshift's
/// res/description/vprsrenderer.h, in no SDK).
static const maxon::Int32 REDSHIFT_RENDERER_IMAGE_UNITS_SCALE = 1803;

/// Its render settings "Units": the Mode (REDSHIFT_RENDERER_INTEGRATION_UNIT_MODE: Disabled 0, Custom 1,
/// Project Settings 2, the default of a new document) and the Export Units of "Custom"
/// (REDSHIFT_RENDERER_INTEGRATION_EXPORT_SCALE, a UnitScaleData, 1 cm by default). Redshift's
/// res/description/vprsrenderer.h (2027), in no SDK.
static const maxon::Int32 REDSHIFT_RENDERER_UNIT_MODE = 25007;
static const maxon::Int32 REDSHIFT_RENDERER_UNIT_MODE_DISABLED = 0;
static const maxon::Int32 REDSHIFT_RENDERER_UNIT_MODE_CUSTOM = 1;
static const maxon::Int32 REDSHIFT_RENDERER_UNIT_MODE_PROJECT = 2;
static const maxon::Int32 REDSHIFT_RENDERER_EXPORT_SCALE = 25002;

/// Redshift's Object tag (the SDK's Trsobject, ge_prepass.h), its Object ID and the switch that makes
/// the Object ID count (Redshift's res/description/trsobject.h, the same in 2024 to 2027; in no SDK).
static const maxon::Int32 REDSHIFT_OBJECT_TAG = 1036222;
static const maxon::Int32 REDSHIFT_OBJECT_OBJECTID_ID = 1000;
static const maxon::Int32 REDSHIFT_OBJECT_OBJECTID_OVERRIDE = 1999;
/// The tag's displacement Mode (REDSHIFT_OBJECT_GEOMETRY_DISPLACEMENT_MODE: Off 0, Render Settings 1, On 2;
/// Redshift's res/description/trsobject.h, 2027).
static const maxon::Int32 REDSHIFT_OBJECT_DISPLACEMENT_MODE = 5014;
static const maxon::Int32 REDSHIFT_OBJECT_DISPLACEMENT_MODE_ON = 2;

/// The three switches of each object in a Redshift light's list (REDSHIFT_LIGHT_EXCLUSION_LIST, read with
/// InExcludeData::GetFlags): what the light lights, the shadows it throws, and the objects below the listed
/// one (Redshift's res/description/orslight.res, 2027: NUM_FLAGS 3, INIT_STATE 7; in no SDK).
static const maxon::Int32 REDSHIFT_LIST_ILLUMINATION = 1;
static const maxon::Int32 REDSHIFT_LIST_SHADOWS = 2;
static const maxon::Int32 REDSHIFT_LIST_HIERARCHY = 4;

/// A Redshift light's units "Luminous Intensity (cd)" and "Luminous Exitance (lm/m2)": values of
/// REDSHIFT_LIGHT_PHYSICAL_UNITSTYPE that no SDK's orslight.h lists (Redshift's own description of the
/// light lists them in Cinema 4D 2026 and 2027; Redshift 3.6.04, Cinema 4D 2024's, has neither).
static const maxon::Int32 REDSHIFT_UNITS_LUMINOUS_INTENSITY = 5;
static const maxon::Int32 REDSHIFT_UNITS_LUMINOUS_EXITANCE = 6;
/// A Redshift light's ray contribution "Camera" (REDSHIFT_LIGHT_CAMERA_RAY_CONTRIBUTION_SCALE in
/// orslight.h from the 2025 SDK on; not in the 2024 SDK).
static const maxon::Int32 REDSHIFT_CAMERA_CONTRIBUTION = 10042;

} // namespace cyclesrender

#endif // CYCLESRENDER_REDSHIFT_IDS_H__
