// SPDX-License-Identifier: Apache-2.0
// The Render Settings of Cycles Render, and how they reach Blender.

#ifndef CYCLESRENDER_RENDER_SETTINGS_MAP_H__
#define CYCLESRENDER_RENDER_SETTINGS_MAP_H__

#include "c4d.h"

#include "../scene/scene_exporter.h"
#include "../session/bridge_client.h"

namespace cyclesrender
{

/// A render pass the user can switch on in the Render Settings.
struct RenderPass
{
	maxon::Int32 id;              ///< parameter id (a checkbox) in vpcyclesrender.h
	const maxon::Char* name;      ///< Blender's name of the pass; also the name of the layer in Cinema 4D
	const maxon::Char* property;  ///< the property of Blender's view layer that switches it on
};

/// Every pass there is, in the order of the Render Settings.
maxon::Block<const RenderPass> GetRenderPasses();

/// Writes the default of every setting into a new renderer's container.
void InitRenderSettings(cinema::BaseContainer& data);

//----------------------------------------------------------------------------------------
/// Sends a `settings` request: engine, device, the document's render colour space, and every
/// setting that maps onto a Blender property (the table in render_settings_map.cpp).
/// @param[out] warnings  Receives what the bridge could not honour, in words for the user.
//----------------------------------------------------------------------------------------
maxon::Result<void> SendRenderSettings(BridgeClient& client, const cinema::BaseContainer& data, const cinema::BaseDocument* doc,
	cinema::BaseThread* cancel, maxon::BaseArray<maxon::String>& warnings);

//----------------------------------------------------------------------------------------
/// Writes the same `settings` request into @p request without sending it. The top-level
/// object is left open, as BridgeClient::Request() expects.
/// @param[in] engineOverride  CYCLES_ENGINE_CYCLES or CYCLES_ENGINE_EEVEE to use that engine
///                            whatever the settings say; NOTOK to follow the settings.
/// @return                    The engine the request selects.
//----------------------------------------------------------------------------------------
maxon::Int32 WriteRenderSettings(JsonWriter& request, const cinema::BaseContainer& data, const cinema::BaseDocument* doc,
	maxon::Int32 engineOverride, maxon::BaseArray<maxon::String>& warnings);

/// Hides the groups and settings of the engine that is not chosen, and in the Simple layout
/// those marked as advanced. For GetDDescription().
void HideSettingsOfOtherEngine(const cinema::BaseContainer& data, cinema::Description* description);

/// False when the setting depends on another one that is switched off. For GetDEnabling().
maxon::Bool IsSettingEnabled(const cinema::BaseContainer& data, maxon::Int32 id);

/// The Cycles Render settings of the document's active render settings, or the defaults when
/// Cycles Render is not set up there.
cinema::BaseContainer GetDocumentRenderSettings(cinema::BaseDocument* doc);

/// The settings that steer the scene export rather than a Blender render property.
ExportSettings GetExportSettings(const cinema::BaseContainer& data, maxon::Int32 width, maxon::Int32 height);

} // namespace cyclesrender

#endif // CYCLESRENDER_RENDER_SETTINGS_MAP_H__
