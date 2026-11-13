// SPDX-License-Identifier: Apache-2.0
// Writes a light as a `light` item: Cinema 4D's standard lights and Redshift lights translated,
// the plugin's own Cycles Light object and the Cycles Light tag in Blender's own terms.

#ifndef CYCLESRENDER_LIGHT_EXPORT_H__
#define CYCLESRENDER_LIGHT_EXPORT_H__

#include "c4d.h"

#include "coords.h"
#include "document_colors.h"
#include "scene_writer.h"

namespace cyclesrender
{

/// The scene items a light is limited to (its "Project" tab: include or exclude list).
struct LightLinking
{
	maxon::Bool include = false;                 ///< true: only these are lit; false: all but these
	maxon::BaseArray<maxon::String> itemIds;     ///< ids of `object` and `instancer` items
};

/// Blender's four light types.
enum class BLENDERLIGHT
{
	POINT,
	SUN,
	SPOT,
	AREA
} MAXON_ENUM_LIST(BLENDERLIGHT);

/// Blender's area shapes (`Light.shape`), in the order of the Cycles Light's dropdown.
enum class BLENDERAREA
{
	SQUARE,
	RECTANGLE,
	DISK,
	ELLIPSE
} MAXON_ENUM_LIST(BLENDERAREA);

//----------------------------------------------------------------------------------------
/// A light in Blender's terms, as it is sent (protocol/protocol.md, `light`): lengths in metres,
/// angles in radians, colours in Blender's working space. The defaults are those of a point
/// light Blender 5.2.3 adds. Translated lights fill only the first block; the plugin's own light
/// and the Cycles Light tag also the others, and only then are those fields sent (a light
/// without them gets Blender's defaults, as before they existed).
//----------------------------------------------------------------------------------------
struct BlenderLight
{
	BLENDERLIGHT type = BLENDERLIGHT::POINT;
	maxon::Float64 energy = 10.0;                ///< watts; W/m2 for a sun
	cinema::Vector color = cinema::Vector(1.0);
	maxon::Bool useShadow = true;
	maxon::Float64 radius = 0.0;                 ///< point and spot: Blender's shadow_soft_size
	maxon::Float64 spotSize = maxon::PI * 0.25;  ///< spot: the full cone angle
	maxon::Float64 spotBlend = 0.15;
	BLENDERAREA shape = BLENDERAREA::SQUARE;
	maxon::Float64 size = 1.0;                   ///< area; the width for a rectangle or ellipse
	maxon::Float64 sizeY = 1.0;
	maxon::Float64 angle = 0.0;                  ///< sun: the angular diameter

	/// Blender's Light panel beyond what a translated light has: exposure, normalize,
	/// temperature, soft falloff (point, spot) and spread (area).
	maxon::Bool blenderSettings = false;
	maxon::Float64 exposure = 0.0;
	maxon::Bool normalize = true;
	maxon::Bool useTemperature = false;
	maxon::Float64 temperature = 6500.0;         ///< kelvin
	maxon::Bool softFalloff = true;
	maxon::Float64 spread = maxon::PI;

	/// Cycles' Settings panel (`light.cycles`).
	maxon::Bool cyclesSettings = false;
	maxon::Int32 maxBounces = 1024;
	maxon::Bool multipleImportance = true;
	maxon::Bool shadowCaustics = false;
	maxon::Bool portal = false;                  ///< area only

	/// The light object's ray visibility.
	maxon::Bool rayVisibility = false;
	maxon::Bool visibleCamera = false;
	maxon::Bool visibleDiffuse = true;
	maxon::Bool visibleGlossy = true;
	maxon::Bool visibleTransmission = true;
	maxon::Bool visibleVolumeScatter = true;

	/// Eevee's panels: influence, custom distance, shadow.
	maxon::Bool eeveeSettings = false;
	maxon::Float64 diffuseFactor = 1.0;
	maxon::Float64 specularFactor = 1.0;
	maxon::Float64 transmissionFactor = 1.0;
	maxon::Float64 volumeFactor = 1.0;
	maxon::Bool useCustomDistance = false;
	maxon::Float64 customDistance = 40.0;        ///< metres
	maxon::Bool shadowJitter = false;
	maxon::Float64 shadowOverblur = 10.0;        ///< percent, as Blender stores it
	maxon::Float64 shadowFilter = 1.0;
	maxon::Float64 shadowResolution = 0.001;     ///< metres
	maxon::Bool shadowAbsolute = false;
};

//----------------------------------------------------------------------------------------
/// What Cinema 4D's standard light or Redshift light @p light is in Blender's terms (only the
/// first block of BlenderLight is filled). Warnings go to @p writer when one is given.
///
/// Cinema 4D's standard lights have no physical unit: at 100 % brightness a white diffuse
/// surface facing the light renders as 1.0, at any distance unless a falloff is set. Blender
/// lights always fall off with the square of the distance, so a light is given the power
/// that reproduces Cinema 4D's brightness at one reference distance: the light's
/// "Radius/Decay" value (500 cm by default), whether its falloff is switched on or not.
/// With "Inverse Square" falloff the two then agree at every distance. The constants are
/// derived in docs/reference/c4d-sdk.md, section "Light conversion". A Redshift light
/// (Orslight) is physical and converts exactly: infinite, point, spot and area lights in the
/// unit "Image".
///
/// @param[in] colors      The document's colour settings: a standard light's colour is a colour
///                        parameter (DocumentColors::Input). A Redshift light's is sent as stored.
/// @param[in] multiplier  The "Light Multiplier" render setting.
/// @return  False when the light emits nothing.
//----------------------------------------------------------------------------------------
maxon::Bool TranslateLight(const cinema::BaseObject* light, const Coords& coords, const DocumentColors& colors, maxon::Float64 multiplier,
	BlenderLight& out, SceneWriter* writer);

//----------------------------------------------------------------------------------------
/// Exports the light @p light, whose global matrix is @p mg, under the id @p id: a standard
/// light, a Redshift light (both through TranslateLight, and changed by a Cycles Light tag on
/// them: lights/light_tag.h) or the plugin's Cycles Light object (its values as they are).
///
/// @param[in] multiplier  The "Light Multiplier" render setting, applied to every light.
/// @return  False when the light emits nothing and was skipped.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking = nullptr);

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_EXPORT_H__
