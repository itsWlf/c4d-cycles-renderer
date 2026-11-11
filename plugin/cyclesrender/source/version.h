// SPDX-License-Identifier: Apache-2.0
// The version of the Cycles Render plugin.
//
// A release consists of this plugin and the bridge (the Blender extension in bridge/), built
// from the same sources and carrying the same number. The number stands in three places that
// tools/make_release.ps1 checks against each other: here, `version` in
// bridge/blender_manifest.toml, and BRIDGE_VERSION in bridge/c4d_bridge/__init__.py.
//
// The number says nothing about the wire protocol: whether a plugin and a bridge can talk is
// decided by PROTOCOL_VERSION alone (session/protocol.h).

#ifndef CYCLESRENDER_VERSION_H__
#define CYCLESRENDER_VERSION_H__

#define CYCLESRENDER_VERSION "0.1.0"

#endif // CYCLESRENDER_VERSION_H__
