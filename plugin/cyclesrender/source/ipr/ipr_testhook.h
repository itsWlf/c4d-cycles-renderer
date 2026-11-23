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
/// object_tag|light|light_tag|fog|environment|world_material|scatter|proxy runs an entry of the window's "Add" menu (ipr_add.h): {"added", "message"};
/// add follow runs FollowAddedCameras() as the message plugin does after a change: {"views_set", "switches"}.
/// devices [find|type <n>|use <index> <0|1>|reset] works on the Preferences page's Cycles Render
/// Devices (prefs/devices.h; "find" starts a Blender as the page's button does) and answers with
/// them (DescribeDeviceSettings).
/// Measuring and checking the sync: trace on|off switches the IPR's timeline
/// (ipr_trace.h) and trace hands it over {"trace": [[time, name, value], ...], "now"}; verify exports
/// the active document whole and compares it with what Blender holds (IprController::VerifySync:
/// {"items", "differences"}); sources lists what each mesh leaf is made from
/// (SceneExporter::GetSourceListing); fognoise answers what the last export of a Cycles Fog with a noise
/// measured ({"cells", "sample_ms", "save_ms", "reused", "type", "seed"}: scene/fog_export.h); scatter [name] answers what
/// the Cycles Scatter object of that name in the active document (without a name: the active object) computed at its last
/// evaluation ({"found", "count", "candidates", "placed", "hidden", "triangles", "effectors_run", "fields_sampled",
/// "distribute_ms", "effector_ms", "evaluations", "builds", "stale", "sources", "skipped", "warnings", and per copy, in the object's axes, "positions" (x, y, z),
/// "axes" (X, Y and Z axes, 9 numbers), "colors" (MoGraph's, r, g, b), "copy_sources" (the index into "sources")}:
/// objects/scatter_object.h); proxy [name] answers what the Cycles Proxy object of that name holds of its file
/// ({"found", "proxy": {"state", "status", "file", "stamp", "blender", "entries", "box", "source"}}:
/// scene/proxy_export.h, DescribeProxy) and proxyread [name] first reads the file as its button "Read File" does
/// (starts a Blender; adds "read" or "error"). The status also has meshes_exported, meshes_kept, export_ms
/// and mesh_ms of the last export.
/// Blender versions: versioned <code> exports the active document as for a Blender of
/// that version (BlenderVersionCode(), 502 for 5.2), whatever Blender is installed, and answers
/// {"materials": the material items as they would be sent, "warnings"}: what that version lacks
/// is left out (materials/cycles_node_material.cpp).
/// Light values in a modal loop: lights runs IprController::FollowLights(), what the
/// scene hook has the window do while Cinema 4D is in a modal loop (only the lights, without a walk
/// of the document); the status's changes then counts the light items sent or removed, lights_ms
/// says how long writing and comparing them took, and lights_followed how many lights it writes.
/// Navigating from the picture: navigate orbit|pan|dolly <x> <y> <dx> <dy> [steps] [ctrl]
/// [cancel] runs a navigating drag as the window's picture does (ipr_navigation.h: the same checks,
/// Begin, a Step per step and End or, with "cancel", Cancel as on Escape), without the window: it
/// begins at the image point (x, y) of the size the IPR renders at, with the picture at 100 %, and
/// the mouse goes (dx, dy) screen pixels, right and down positive; the camera is sent after each
/// step. It answers {"refused" (empty when it navigated, else the status bar's text), "camera",
/// "editor_camera", "picked", "pivot", "pick_ms", "prefs" (the navigation preferences it followed)}.
/// The Preferences: blender [<path>|none|detect] stores the Blender as the page's
/// field does (none: empty; detect: the Detect button) and answers {"path", "in_use" (the path
/// detection gives while none is stored), "status" (the page's line), "problem" (GetBlenderProblem:
/// why none can be started; empty when one can), "detected"}; defaultengine [<0|1>] sets the
/// page's Default Engine and answers {"default_engine"}. The status has blender_missing
/// (IprController::IsBlenderMissing) and window_message (the text on the window's picture).
/// What renders, and what is left to Cinema 4D: the status has device, device_engine,
/// device_type, device_names, device_threads, display_gpu (Blender's reply to the last `settings`
/// request, IprWorker::GetDeviceInfo), device_text (the status line's words for it) and
/// window_device_text (what the window's line shows), threads_free (the preference) and
/// logical_processors, c4d_priority, blender_priority (the classes FollowPriority saw and set, ranks of
/// platform/process_stats.h), blender_priority_now (Blender's class read now), blender_pid, state_block (the name of
/// the frame block's state block, for a test that reads the newest published frame itself) and shown_frame (the id of
/// the frame the window shows; 0 without a window); threadsfree [<n>] sets
/// the page's "CPU Threads Left Free" and answers {"threads_free"}.
/// A change in progress: redrawn tells the controller of a redraw (IprController::NoteRedraw,
/// what EVMSG_DOCUMENTRECALCULATED does: the next tick exports only if the document's change counters
/// moved), step runs IprController::FollowStep(true) (a tool's step: what moved is sent alone, or the
/// scene whole), evaluated runs FollowStep(false) (a step inside an evaluation, as the scene hook's:
/// never the whole scene). The status has optimized_updates (the preference), gated (redraws that
/// exported nothing), alone (updates of single items sent), settled (settle exports), all three since
/// the IPR started; settle_pending, whole_needed (the last step could not be sent alone), alone_ms and
/// alone_objects (of the last step: its time, the `object` items it wrote), objects_followed and
/// nodes_followed (what a step can write and what it compares), and meshes_by_data_hash (meshes of the
/// last export judged by Cinema 4D's data hashes, scene/mesh_export.h).
/// Meshes in a change in progress: unevaluated runs FollowStep(false, false) (a step as a
/// frame's call makes it in a modal loop: no cache of the document is read, so nothing is collected
/// again), quiet <seconds> sets the time until a step may collect objects again (the guard after a step
/// that took long; 0: at once). The status has alone_owners, alone_meshes and alone_kept (of the last
/// step: the objects of the document collected again, the meshes exported for them and the meshes kept)
/// postponed (steps left out since the IPR started, by that guard or for want of readable caches),
/// whole_reason (why the last step could not go alone: IprController::WHOLE, 0 when it could) and
/// mesh_quiet_ms (for how long from now the guard leaves out steps that collect).
/// With the window open the status also has image_rect (where the picture is on the screen: left,
/// top, width, height) and image_center (the image point in its middle), for tests that post mouse
/// messages to it.
//----------------------------------------------------------------------------------------
maxon::Bool RunIprTestCommand();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_TESTHOOK_H__
