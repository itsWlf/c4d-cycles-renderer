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

/// The scene items a light is limited to (its "Project" tab: include or exclude list, and the
/// Cycles Object tags' lists): which it lights, and which cast its shadows. Without shadowsApart the
/// shadows follow the lighting, as Cinema 4D's own lists have it.
struct LightLinking
{
	maxon::Bool lightLinked = true;              ///< the lighting is limited (the item's `linking`)
	maxon::Bool include = false;                 ///< true: only these are lit; false: all but these
	maxon::BaseArray<maxon::String> itemIds;     ///< ids of `object` and `instancer` items
	maxon::Bool shadowsApart = false;            ///< the shadows have lists of their own (the item's `shadow_linking`)
	maxon::Bool shadowInclude = false;           ///< true: only these cast its shadows; false: all but these
	maxon::BaseArray<maxon::String> shadowItemIds;
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
/// without them gets Blender's defaults).
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
	/// Area: Blender's spread (full angle). Sent when blenderSettings is set or it is narrower than
	/// Blender's default (a Redshift area light's "Spread").
	maxon::Float64 spread = maxon::PI;
	/// Area: the light shines to both sides (a Redshift area light's "Bi-Directional"). Blender's area
	/// light shines to one, so a second item, BackLightId(), shines the other way.
	maxon::Bool twoSided = false;

	/// Blender's Light panel beyond what a translated light has: exposure, normalize,
	/// temperature and soft falloff (point, spot).
	maxon::Bool blenderSettings = false;
	maxon::Float64 exposure = 0.0;
	maxon::Bool normalize = true;
	maxon::Bool useTemperature = false;
	maxon::Float64 temperature = 6500.0;         ///< kelvin
	maxon::Bool softFalloff = true;              ///< point, spot; also sent off for a translated sphere

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
/// (Orslight) is physical and converts exactly where Blender has the same light (measured with
/// Redshift 2027.0.0, docs/reference/c4d-sdk.md, "Redshift lights"): infinite, point, spot and
/// area lights (rectangle, disc or ellipse, sphere) in every intensity unit, with exposure,
/// "Normalize Intensity", the object's scale on its size, its colour temperature as Redshift
/// computes it in the render space, its camera visibility and "Bi-Directional" (twoSided), under
/// each of Redshift's Units Modes with its "Image Units Scale" and Export Units (since 0.3.0).
/// Approximated, each named in the console: "Spread" (Blender's profile fitted to Redshift's;
/// Blender's edge is soft), the decays "None" and "Linear" (matched at one distance) and a
/// cylinder (a sphere as bright from its side); not translated, named: a mesh shape, a texture,
/// the spot's falloff curve, the sun's sky, portals and IES profiles.
///
/// Shadows: a standard light always casts them, whatever its "Shadow" type, also
/// "None", Cinema 4D's default (the Standard renderer casts none then); a Cycles Light tag's
/// Settings tab turns them off. The type sets their softness through the light's size (radius,
/// a sun's angle): None and Raytraced sharp; Area from the area shape or the Infinite Angle, as
/// physical as Cinema 4D's; Shadow Maps (Soft) from the map's size and sample radius, an
/// approximation matched at the light's reference distance. An area light keeps its own size.
/// A Redshift light follows its own "Shadow" switch.
///
/// @param[in] colors      The document's colour settings: a standard light's colour is a colour
///                        parameter (DocumentColors::Input), and so is a Redshift light's (since 0.3.0;
///                        before, it was sent as stored); its colour temperature is made a colour of
///                        the render space, in a document with Basic colour management Redshift's ACEScg
///                        colour in linear Rec.709.
/// @param[in] multiplier  The "Light Multiplier" render setting.
/// @param[in] mg          The light's global matrix: a Redshift area light's size counts with its
///                        scale (measured), as the light's direction is sent without it.
/// @return  False when the light emits nothing.
//----------------------------------------------------------------------------------------
maxon::Bool TranslateLight(const cinema::BaseObject* light, const Coords& coords, const DocumentColors& colors, maxon::Float64 multiplier,
	const cinema::Matrix& mg, BlenderLight& out, SceneWriter* writer);

/// The id of the item that shines a two-sided light's other way (BlenderLight::twoSided), for the
/// light item @p id.
maxon::String BackLightId(const maxon::String& id);

//----------------------------------------------------------------------------------------
/// Whether @p light is switched on by its green check in the Object Manager (the object's deform
/// mode, `ID_BASEOBJECT_GENERATOR_FLAG`): Cinema 4D's light, a Redshift light and the
/// Cycles Light object have it. Measured with the Standard renderer and Redshift (2024.5, 2026.3,
/// 2027.0): a light whose check is off emits nothing, and the check counts for that light only, not
/// for lights below it (docs/reference/c4d-sdk.md, "Lights switched off").
//----------------------------------------------------------------------------------------
maxon::Bool IsLightSwitchedOn(const cinema::BaseObject* light);

/// Redshift's renderer in the document's active render data (its video post, REDSHIFT_RENDERER, which holds
/// Redshift's own render settings and which a scene set up for Redshift keeps when Cycles Render is chosen),
/// or nullptr. Every new document of Cinema 4D 2027 holds one (measured), so it alone does not make a scene
/// one set up for Redshift (ExportWorldItem asks for a Redshift light as well).
const cinema::BaseVideoPost* FindRedshiftRenderer(const cinema::BaseDocument* doc);

//----------------------------------------------------------------------------------------
/// Exports the light @p light, whose global matrix is @p mg, under the id @p id: a standard
/// light, a Redshift light (both through TranslateLight, and changed by a Cycles Light tag on
/// them: lights/light_tag.h) or the plugin's Cycles Light object (its values as they are).
/// A light switched off by its check (IsLightSwitchedOn) is skipped. A two-sided light writes its
/// other side as a second item (BackLightId); otherwise that item is named for removal, so that a
/// light written alone (the IPR during a drag) takes it out of a partial request too.
///
/// @param[in] multiplier  The "Light Multiplier" render setting, applied to every light.
/// @param[in] eeveeHidesLights  The frame is rendered by an Eevee that shows no light to the camera
///                        (Blender 5.0 and 5.1, measured): a light whose Camera visibility is on
///                        is named in a warning.
/// @return  False when the light emits nothing or is switched off, and was skipped.
//----------------------------------------------------------------------------------------
maxon::Result<maxon::Bool> ExportLight(SceneWriter& writer, const Coords& coords, const DocumentColors& colors, const cinema::BaseObject* light,
	const cinema::Matrix& mg, const maxon::String& id, maxon::Float64 multiplier, const LightLinking* linking = nullptr,
	maxon::Bool eeveeHidesLights = false);

/// The first Blender whose Eevee shows a light to the camera when the light's Camera visibility is
/// on, as BlenderVersionCode() (measured: 5.0.0 and 5.1.2 never, 5.2.3 and 5.3.0 as Cycles does).
static constexpr maxon::Int32 EEVEE_LIGHT_VISIBLE_VERSION = 502;

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_EXPORT_H__
