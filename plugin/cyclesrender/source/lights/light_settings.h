// SPDX-License-Identifier: Apache-2.0
// Blender's light settings as parameters of a Cinema 4D node: what the Cycles Light object and the
// Cycles Light tag have in common (the same parameter ids, res/description/ocycleslight.h and
// tcycleslight.h): their defaults, what the Attribute Manager shows of them, and reading them.

#ifndef CYCLESRENDER_LIGHT_SETTINGS_H__
#define CYCLESRENDER_LIGHT_SETTINGS_H__

#include "c4d.h"

#include "../scene/light_export.h"

namespace cyclesrender
{

/// Blender's defaults for a light it adds (Blender 5.2.3, Add > Light, measured in Blender):
/// lengths in document units (1 m is 100 in a document in centimetres), angles in radians.
namespace LIGHTSETTINGS_DEFAULT
{
	static const maxon::Float64 ENERGY = 10.0;               ///< watts (Blender gives a new sun 1 W/m2)
	static const maxon::Float64 TEMPERATURE = 6500.0;
	static const maxon::Float64 SIZE = 100.0;                ///< an area light's 1 m
	static const maxon::Float64 ANGLE = 0.526 * 3.14159265358979323846 / 180.0;
	static const maxon::Float64 SPOT_SIZE = 3.14159265358979323846 / 4.0;
	static const maxon::Float64 SPOT_BLEND = 0.15;
	static const maxon::Float64 SPREAD = 3.14159265358979323846;
	static const maxon::Int32 MAX_BOUNCES = 1024;
	static const maxon::Float64 CUSTOM_DISTANCE = 4000.0;    ///< 40 m
	static const maxon::Float64 SHADOW_OVERBLUR = 0.1;       ///< 10 %
	static const maxon::Float64 SHADOW_FILTER = 1.0;
	static const maxon::Float64 SHADOW_RESOLUTION = 0.1;     ///< 0.001 m
}

/// Sets every light setting of @p data to Blender's default.
void InitLightSettings(cinema::BaseContainer& data);

//----------------------------------------------------------------------------------------
/// Hides in @p description what the type and shape of @p data do not use, and names Power and
/// Color as Blender's panel does for them ("Strength" for a sun, "Tint" with a temperature):
/// Blender's Light, Beam Shape, Settings and Eevee panels (properties_data_light.py and the
/// Cycles add-on's ui.py of Blender 5.2.3).
//----------------------------------------------------------------------------------------
void ShowLightSettings(const cinema::BaseContainer& data, cinema::Description* description);

/// False for a setting that does not count with the other values of @p data, which Blender's
/// panel greys out: the temperature without "Temperature", Overblur without Jitter, the
/// distance without "Custom Distance", the settings a portal does not use.
maxon::Bool IsLightSettingActive(const cinema::BaseContainer& data, maxon::Int32 id);

//----------------------------------------------------------------------------------------
/// Reads the Light tab (type, temperature and colour, power, exposure, normalize and the size and
/// beam of the type) of @p node into @p out, in Blender's terms. The colour is a colour
/// parameter of Cinema 4D (DocumentColors::Input). Sets `out.blenderSettings`.
//----------------------------------------------------------------------------------------
void ReadLightPanel(const cinema::BaseList2D* node, const Coords& coords, const DocumentColors& colors, BlenderLight& out);

/// Reads the Settings tab (Cycles: max bounces, cast shadow, multiple importance, shadow
/// caustics, portal). Sets `out.cyclesSettings`.
void ReadCyclesSettings(const cinema::BaseList2D* node, BlenderLight& out);

/// Reads the Visibility tab (the ray visibility of the light object). Sets `out.rayVisibility`.
void ReadRayVisibility(const cinema::BaseList2D* node, BlenderLight& out);

/// Reads the Eevee tab. Sets `out.eeveeSettings`.
void ReadEeveeSettings(const cinema::BaseList2D* node, const Coords& coords, BlenderLight& out);

/// Writes the Light tab's values of @p values into @p data, lengths back into document units:
/// what the Cycles Light tag's "Take Values from the Light" does. @p values is in Blender's
/// terms, its colour as Cinema 4D stores it (read without DocumentColors).
void StoreLightPanel(cinema::BaseContainer& data, const BlenderLight& values, const Coords& coords);

} // namespace cyclesrender

#endif // CYCLESRENDER_LIGHT_SETTINGS_H__
