// SPDX-License-Identifier: Apache-2.0
// The Output tab of the Render Settings (Blender's Format panel: resolution, percentage, pixel
// aspect) and the pixel aspect every render uses.
//
// Cinema 4D's Output page keeps the values that render: Width, Height (RDATA_XRES, RDATA_YRES)
// and Pixel Aspect (RDATA_PIXELASPECT). Cinema 4D allocates the picture from them and the IPR
// reads them, so they stay the one place a render takes its size from. The Output tab shows
// them in Blender's terms and writes them:
//   Width  = Resolution X x % / 100,  Height = Resolution Y x % / 100,  Pixel Aspect = Aspect X / Aspect Y.
// Its own values are stored in the renderer's settings. When the two disagree (Cinema 4D's page
// was changed since, or the renderer was added to a document of another size) the tab shows what
// Cinema 4D's values give, so it always shows what renders.

#ifndef CYCLESRENDER_OUTPUT_SETTINGS_H__
#define CYCLESRENDER_OUTPUT_SETTINGS_H__

#include "c4d.h"

namespace cyclesrender
{

/// Cinema 4D's pixel aspect (width of a pixel over its height) from render data; 1 when unset
/// or not positive.
maxon::Float64 GetPixelAspect(const cinema::BaseContainer& renderData);

/// True for the parameters of the Output tab (and its line of text), which GetOutputParameter()
/// and SetOutputParameter() answer.
maxon::Bool IsOutputParameter(maxon::Int32 id);

/// What the Output tab shows for @p id: its stored value, or the one Cinema 4D's Output page
/// gives where the two disagree. False when @p id is no parameter of the tab.
/// @param[in] videoPost  The renderer's video post; its render data is found through its document.
maxon::Bool GetOutputParameter(const cinema::BaseList2D* videoPost, maxon::Int32 id, cinema::GeData& value);

/// Stores a value typed into the Output tab and writes Width, Height and Pixel Aspect of the
/// render data that holds @p videoPost. False when @p id is no parameter of the tab.
maxon::Bool SetOutputParameter(cinema::BaseList2D* videoPost, maxon::Int32 id, const cinema::GeData& value);

/// The render data that holds @p videoPost, or nullptr (a video post outside a document).
cinema::RenderData* FindRenderDataOf(const cinema::BaseList2D* videoPost);

} // namespace cyclesrender

#endif // CYCLESRENDER_OUTPUT_SETTINGS_H__
