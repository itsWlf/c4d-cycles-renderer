# SPDX-License-Identifier: GPL-3.0-or-later
"""Cycles Render bridge: runs inside Blender and renders what the Cinema 4D plugin sends.

The package is started by Blender running main.py as a script (see there). The protocol it
speaks is defined in protocol/protocol.md at the repository root.
"""

# The release this bridge belongs to. The same number stands in ../blender_manifest.toml and in
# the plugin's version.h; tools/make_release.ps1 checks the three against each other. It is
# reported to the plugin for information; what decides whether the two can talk is
# protocol.VERSION.
BRIDGE_VERSION = "0.1.0"

# (major, minor) of the Blender versions the bridge is validated against. It wraps a method of
# Cycles' Python class and reads a struct of Blender's by offset, so an unknown version is
# refused at start (main.py). Before adding one, go through "Version coupling" in
# docs/reference/blender.md. The plugin keeps the same list: SUPPORTED_VERSIONS in
# plugin/cyclesrender/source/session/blender_install.cpp.
SUPPORTED_BLENDER = ((5, 2),)
