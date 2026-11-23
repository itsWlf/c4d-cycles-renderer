// SPDX-License-Identifier: Apache-2.0
// The "Cycles Proxy" object: names a Blender file, which Blender links and shows at the object's
// place in the render.

#ifndef CYCLESRENDER_PROXY_OBJECT_H__
#define CYCLESRENDER_PROXY_OBJECT_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the object with Cinema 4D. Called once from PluginStart(). The object holds a .blend
/// file's name (absolute, or relative to the document, with the "Make Relative" and "Make Absolute"
/// buttons of the Cycles Environment's HDRI), what of the file is shown (the file's scene, or one of
/// its scenes or collections by name), whether the file's lights are taken and a frame offset
/// (res/description/ocyclesproxy.h). The plugin never opens the file: "Read File" asks a Blender for
/// the scenes and collections in it and the box each fills (scene/proxy_export.h, ReadProxyFile), and
/// the answer is kept in the object, so the menu and the box are there when the scene is opened
/// without a Blender. In the viewport it is that box, in the object's colour, with a cross at the
/// file's origin; it builds no cache and shows nothing in Cinema 4D's own renderers. It has the green
/// "enabled" check in the Object Manager. The exporter writes it as a `proxy` item at its place
/// (scene/scene_exporter.h). The file is reported as an asset of the scene (Save Project with
/// Assets, Team Render); what the file itself refers to (its textures, further linked files) is not.
cinema::Bool RegisterProxyObject();

} // namespace cyclesrender

#endif // CYCLESRENDER_PROXY_OBJECT_H__
