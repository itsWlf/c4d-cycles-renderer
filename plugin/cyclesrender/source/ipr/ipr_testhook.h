// SPDX-License-Identifier: Apache-2.0
// Lets test scripts drive the IPR without its window.

#ifndef CYCLESRENDER_IPR_TESTHOOK_H__
#define CYCLESRENDER_IPR_TESTHOOK_H__

#include "c4d.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// Runs the IPR command a test left in the plugin's preferences container and stores the
/// answer there. Only active with g_cyclesRenderIprTest=true. Returns false when there was
/// no command. Protocol, from Python (see tests/c4d/lib/crtest.py, ipr()):
///
///   bc = c4d.BaseContainer(); bc[9000] = "start"
///   c4d.plugins.SetWorldPluginData(1073816, bc)
///   c4d.GePluginMessage(1073816, None)
///   answer = c4d.plugins.GetWorldPluginData(1073816)[9001]     # JSON text
///
/// Commands: start, stop, pause, reload, tick, changed, status, kill, free,
/// pass <index>, engine <-1|0|1>, scale <factor>, fitwindow <0|1>, winsize <w>x<h> (the window's picture area, when there is no window), lock <0|1>, window <0|1>,
/// camera render|auto|<name of a camera object>, frame <file> (writes the newest frame as
/// "CRF1" raw floats), display <file> (the same frame as the window shows it, DisplayTransform,
/// display values 0..255 written as 0..1; works without a window), picture <file> (the frame
/// as the window's "Save Image" writes it, a 32-bit EXR; without a window), and the window's commands
/// (ipr_dialog.h): open, zoom <percent>, type_zoom <percent>,
/// fit, snapshot, save <file>. Every command answers with the status: state, message, idle, frame,
/// view (state in Blender), sample, samples, width, height, elapsed, scenes_sent, requests_sent, changes,
/// engine, pass, pass_count, ram, ram_total, vram, vram_total, mean (RGB of the newest frame), and
/// for the window zoom and zoom_field (the picture's zoom and what its zoom field holds, 1 = 100 %),
/// and bakes (shaders baked by this process). Two commands answer otherwise: bakes [check|cleanup]
/// gives {"bakes": n, "store": the baked files (materials/baked_files.h, DescribeBakedFiles)}, and
/// curve <node> <port> [<knots> [<range>]] works on a curve setting (kind 'k') of the Cycles node
/// with that id in a material of the active document, because Python cannot make a
/// maxon::SplineMapper: with knots ("x,y,kind[,left tangent x,y,right tangent x,y];...", kind b
/// Bezier, l linear, c cubic, or "none"; range "min x,min y,max x,max y", 0..1 if left out) it
/// first sets the port as Cinema 4D's spline control does; it answers {"material", "range",
/// "area" (Blender's, nodespace/cycles_nodes.h), "tension", "knots" ([x, y, interpolation,
/// tangents, flags], sorted), "c4d" (SplineMapper::GetPoint at 1001 even positions across the
/// range and knots, in the control's coordinates), "sent" (the points the exporter sends, in
/// Blender's), "problems"}. Three more answer {"material", ...} for the first material of Cycles
/// nodes of the active document: imagedrop <file> (what a drop of an image on the Node Editor
/// does with what the node space names: a node of NODESPACE::IMAGENODEASSETID with the file in
/// its IMAGENODEPORTS; "asset", "ports", "ports_found", "node"), pathcommand relative|absolute
/// <node id> (runs that button's command through maxon::CommandClasses: "result") and path
/// <node id> (both also give the file's "scheme" and "path"). And add material|camera|camera_tag|
/// object_tag|light|light_tag runs an entry of the window's "Add" menu (ipr_add.h): {"added", "message"}.
/// devices [find|type <n>|use <index> <0|1>|reset] works on the Preferences page's Cycles Render
/// Devices (prefs/devices.h; "find" starts a Blender as the page's button does) and answers with
/// them (DescribeDeviceSettings).
//----------------------------------------------------------------------------------------
maxon::Bool RunIprTestCommand();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_TESTHOOK_H__
