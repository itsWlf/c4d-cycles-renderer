// SPDX-License-Identifier: Apache-2.0
// Writes the `world` item: what the scene sees where there is no object. Also what the Cycles
// Environment object (objects/environment_object.h) takes from Blender's node table.

#ifndef CYCLESRENDER_ENVIRONMENT_EXPORT_H__
#define CYCLESRENDER_ENVIRONMENT_EXPORT_H__

#include "c4d.h"

#include "../materials/material_export.h"
#include "../nodespace/cycles_nodes.h"
#include "coords.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// One source of the world: what the camera sees, what lights the scene, or the world's fog.
//----------------------------------------------------------------------------------------
enum class WORLDPART
{
	NONE,                ///< nothing fills the role: black for the camera, no light, no fog
	SKY,                 ///< a Sky object, with the material of its texture tags
	DOME,                ///< a Redshift dome light
	ENVIRONMENT,         ///< a Cycles Environment object
	BACKGROUND,          ///< Cinema 4D's Background object: seen by the camera only, behind every other environment
	FOG                  ///< Cinema 4D's Environment object with its fog on: the world's fog only (0.3.0, F13 W28)
} MAXON_ENUM_LIST(WORLDPART);

struct WorldPart
{
	WORLDPART kind = WORLDPART::NONE;
	const cinema::BaseObject* object = nullptr;
	cinema::Matrix mg;                                 ///< its global matrix
	cinema::BaseMaterial* material = nullptr;          ///< a Sky's or a Background's material
};

/// The world colour of new Render Settings (Blender's default world, at strength 1): the world of a scene without an
/// environment, except in a scene set up for Redshift (ExportWorldItem).
static const cinema::Vector RENDER_SETTINGS_WORLD_COLOR = cinema::Vector(0.05);

//----------------------------------------------------------------------------------------
/// Writes the `world` item from its parts (the scene walk chooses them, scene/scene_exporter.h):
///
/// - With no part for the camera and none for the light, the plain @p defaultColor at
///   @p defaultStrength of the Render Settings, for both; in a scene set up for Redshift (Redshift's
///   renderer in the render data, FindRedshiftRenderer, and a Redshift light switched on) black while those
///   are the new settings' (RENDER_SETTINGS_WORLD_COLOR at strength 1), as Redshift renders a scene without
///   an environment (since 0.3.0, named in the console).
/// - With one environment for both, that environment.
/// - With two, the two joined by a Light Path node: camera rays see @p camera, every other ray (diffuse,
///   glossy, transmission, also a reflection the camera sees) sees @p lighting. A role nothing fills is
///   black (a Background of strength 0).
/// - @p fog (a Cycles Environment whose World Fog is on) adds a Principled Volume on the World Output's
///   Volume socket.
///
/// A Sky object: its material's luminance channel (or colour channel, if luminance is off) becomes the
/// world, a bitmap there a spherical panorama turned with the Sky. A Background object (camera only): its
/// material's colour and luminance channels added, unlit. A Redshift dome light: its map as a spherical
/// panorama turned with the light, times its Intensity and 2^exposure; without a map, its colour times its
/// Intensity. A Cycles
/// Environment by its type: Color, a Background of the colour and strength; Sky Texture, Blender's Sky
/// Texture with the object's settings (the sun: GetEnvironmentSunDirection; a setting the Blender in use
/// lacks, by the node table's versions, is left out with a warning); HDRI, the file (found as a bitmap's:
/// next to the document, its "tex" folder, the search paths) as an Environment Texture with the object's
/// colour space, projection and interpolation, turned with the object as a Sky's panorama is, times the
/// strength; Material, the world of the Cycles world material on the object's texture tag. Colours go
/// through the document's colour settings (MaterialExporter::GetColors) like any other colour parameter,
/// a Redshift dome's too (since 0.3.0: Redshift decodes it in a Basic document, measured).
///
/// @param[out] images  If given: the ids of the `image` items the world reads, joined by ";".
//----------------------------------------------------------------------------------------
maxon::Result<void> ExportWorldItem(MaterialExporter& materials, const WorldPart& camera, const WorldPart& lighting, const WorldPart& fog,
	const cinema::Vector& defaultColor, maxon::Float64 defaultStrength, maxon::String* images = nullptr);

/// One setting of the Cycles Environment object that is a setting of one of Blender's nodes: its
/// parameter id (res/description/ocyclesenvironment.h) and the node and setting in the node table.
struct EnvironmentNodeSetting
{
	maxon::Int32 id;
	const maxon::Char* node;          ///< Blender's node type: ShaderNodeTexSky, ShaderNodeTexEnvironment
	const maxon::Char* identifier;    ///< Blender's name of the setting
};

/// Every such setting: the Sky Texture's twelve and the HDRI's colour space, projection and
/// interpolation. Their names, options, defaults and the Blender each exists from are the node table's
/// (nodespace/cycles_nodes_table.h, generated by tools/make_nodes.py).
maxon::Block<const EnvironmentNodeSetting> GetEnvironmentNodeSettings();

/// The node table's entry of @p setting, or nullptr when the table has no such setting.
const CyclesProperty* FindEnvironmentProperty(const EnvironmentNodeSetting& setting);

/// The options of a setting of kind 'e' as the table lists them: Blender's identifier, the label, and
/// the first Blender that has the option (0: since the setting). A stored option is its position here.
struct EnvironmentOption
{
	maxon::String identifier;
	maxon::String label;
	maxon::Int32 since = 0;
};
maxon::Result<maxon::BaseArray<EnvironmentOption>> GetEnvironmentOptions(const CyclesProperty& property);

/// Whether @p environment is switched on by its green check (its deform mode, as the Cycles Light).
maxon::Bool IsEnvironmentSwitchedOn(const cinema::BaseObject* environment);

/// The roles a Cycles Environment asks for (all false while its green check is off): seen by the camera,
/// lighting the scene, and its World Fog. Of the environments that ask for a role, the highest in the
/// Object Manager fills it (scene/scene_exporter.h).
struct EnvironmentRoles
{
	maxon::Bool camera = false;
	maxon::Bool lighting = false;
	maxon::Bool fog = false;
	maxon::Bool operator ==(const EnvironmentRoles& other) const { return camera == other.camera && lighting == other.lighting && fog == other.fog; }
	maxon::Bool operator !=(const EnvironmentRoles& other) const { return !(*this == other); }
};
EnvironmentRoles GetEnvironmentRoles(const cinema::BaseObject* environment);

/// The density a new World Fog starts with, per metre: a twentieth of what a Cycles Fog box starts with,
/// because the world's fog has no end: 5 % of the light is lost over 5 m, 39 % over 50 m
/// (docs/reference/blender.md, "The Cycles Environment object", world fog).
static const maxon::Float64 ENVIRONMENT_DEFAULT_FOG_DENSITY = 0.01;

/// The world material of a Cycles Environment of the Material type: the material of its last texture tag
/// that is a Cycles world material (materials/cycles_node_material.h, IsCyclesWorldMaterial), as a later
/// texture tag covers an earlier one; nullptr when there is none. @p count, if given, gets how many there are.
const cinema::BaseMaterial* GetEnvironmentWorldMaterial(const cinema::BaseObject* environment, maxon::Int* count = nullptr);

/// The light linked as the Sky Texture's sun, or nullptr when none is linked, the link is broken, or it
/// names something that is no light (then @p notALight is set): Cinema 4D's light, a Redshift light or a
/// Cycles Light.
const cinema::BaseObject* GetEnvironmentSunLight(const cinema::BaseObject* environment, const cinema::BaseDocument* doc, maxon::Bool& notALight);

//----------------------------------------------------------------------------------------
/// The sun of a Sky Texture of @p environment at @p mg, in Cinema 4D's world axes (unit length):
/// - with a light linked (GetEnvironmentSunLight), where that light shines from: against its +Z axis, in
///   the world; the object's own rotation then plays no part;
/// - otherwise the settings' Sun Elevation and Sun Rotation (Blender's) in the object's axes, turned with
///   the object. Blender's sky has no tilt: only the sun turns, the horizon stays level.
/// For Preetham and Hosek / Wilkie the same sun is sent as the node's sun direction.
//----------------------------------------------------------------------------------------
cinema::Vector GetEnvironmentSunDirection(const cinema::BaseObject* environment, const cinema::Matrix& mg, const cinema::BaseDocument* doc);

} // namespace cyclesrender

#endif // CYCLESRENDER_ENVIRONMENT_EXPORT_H__
