// SPDX-License-Identifier: Apache-2.0
// Developer self-test of the session layer, run inside Cinema 4D.

#ifndef CYCLESRENDER_SELFTEST_H__
#define CYCLESRENDER_SELFTEST_H__

#include "c4d.h"

namespace cyclesrender
{

/// Starts Blender, exchanges a ping with a 1 MiB blob and shuts Blender down, as many times as
/// the setting g_cyclesRenderSelfTest says, alternating background and windowed Blender.
/// Writes one line per cycle to the console and returns true when every cycle passed.
/// Triggered from tests with GePluginMessage(<renderer id>, ...); see tests/c4d/session_selftest.py.
maxon::Bool RunSessionSelfTest();

} // namespace cyclesrender

#endif // CYCLESRENDER_SELFTEST_H__
