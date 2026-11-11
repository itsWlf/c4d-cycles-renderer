# SPDX-License-Identifier: Apache-2.0
# Module configuration for the Cycles Render plugin.
#
# Built by pointing the Cinema 4D SDK at this repository's plugin folder:
#   cmake -S <sdk> -B <repo>/build/plugin -DMAXON_SDK_MODULES_DIR=<repo>/plugin ...
# tools/build_plugin.ps1 does exactly that; docs/development.md explains the set-up.

set(target_base_name cyclesrender)
set(target_platforms windows)

# apis:     the SDK frameworks whose headers this module may include.
# moduleid: a reverse-domain string that is unique to this module. A fork that is published
#           under another name needs its own, as it needs its own plugin ids (source/ids.h).
set(target_settings [=[
{
  "apis" :
  [
    "asset.framework",
    "cinema.framework",
    "cinema_hybrid.framework",
    "core.framework",
    "corenodes.framework",
    "exchange.framework",
    "graph.framework",
    "image.framework",
    "nodes.framework",
    "nodespace.framework",
    "volume.framework"
  ],
  "c4d" : true,
  "moduleid" : "com.wolfnimations.c4dcyclesrender",
  "stylecheck.level" : 3,
  "type" : "DLL"
}
]=])

# SDK modules are built without RTTI and exceptions; errors go through maxon::Result.
set(enable_rtti OFF)
set(enable_exception_handling OFF)

set(source_directories source res)

MaxonTargets_ProcessCinemaTargetVars()
