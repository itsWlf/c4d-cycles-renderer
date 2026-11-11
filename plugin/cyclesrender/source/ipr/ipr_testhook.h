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
/// "CRF1" raw floats), and the window's commands (ipr_dialog.h): open, zoom <percent>, fit,
/// snapshot, save <file>. Every command answers with the status: state, message, idle, frame,
/// view (state in Blender), sample, samples, width, height, elapsed, scenes_sent, requests_sent, changes,
/// engine, pass, pass_count, ram, ram_total, vram, vram_total, mean (RGB of the newest frame).
//----------------------------------------------------------------------------------------
maxon::Bool RunIprTestCommand();

} // namespace cyclesrender

#endif // CYCLESRENDER_IPR_TESTHOOK_H__
