# SPDX-License-Identifier: Apache-2.0
<#
Runs the project's tests and prints one line per test: all of them (the baseline), the ones a
change needs (-Changed), or the ones named (-Only).

The tests themselves (the folder tests\) are kept locally and are not part of the repository;
without them this script has nothing to run. See docs\development.md, "Tests", for when to run
which: only the tests relevant to it for every piece of work (-Only), the whole baseline before
every version release.

Which versions (the maintainer's rule, 2026-11-16): every run is Cinema 4D 2027 with Blender 5.2
unless told otherwise, the baseline, -Changed and -Only alike; nothing here loops over versions.
The baseline before a release is that pair. Before a major release the baseline also runs on each other
Cinema 4D version with Blender 5.2, and on each other Blender version with Cinema 4D 2027, one run
after the other (never two at once: they share the output folder, the GPU and the plugin binary):
  foreach ($y in "2024","2025","2026") { .\tools\run_tests.ps1 -C4D $y > baseline_c4d$y.txt }
  foreach ($v in "5.0","5.1","5.3") { .\tools\run_tests.ps1 -SkipBuild -Blender $v > baseline_blender$v.txt }

  .\tools\run_tests.ps1              build, bridge tests, then every Cinema 4D test (the baseline)
  .\tools\run_tests.ps1 -SkipBuild   the same without rebuilding the plugin
  .\tools\run_tests.ps1 -Only lights,materials
  .\tools\run_tests.ps1 -Changed     the tests for the files that differ from the last commit
  .\tools\run_tests.ps1 -Changed -DryRun   only print what -Changed would run, and why
  .\tools\run_tests.ps1 -Changed -Files bridge/c4d_bridge/scene/lights.py   as if only that file had changed
  .\tools\run_tests.ps1 -Alone       every Cinema 4D test in a start of its own (the old way)
  .\tools\run_tests.ps1 -Blender 5.0 the baseline with another supported Blender (a version from the
                                     installer's usual place, or the path of a blender.exe)
  .\tools\run_tests.ps1 -C4D 2026    the baseline in another supported Cinema 4D (a version from the
                                     installer's usual place, or the folder of an installation)

-Blender decides the Blender of every test: the bridge tests start it, and the Cinema 4D tests get
it as g_cyclesRenderBlender (the window tests too). The default is Blender 5.2, the long-term
support release; the other versions get the baseline before a major release (above).
docs\development.md, "Tests", says which tests expect something different per version and why.

-C4D decides the Cinema 4D of every Cinema 4D test, and with it the plugin build: the build of
that version (tools\build_plugin.ps1 -Version <year>, build\plugin for 2027, build\plugin-<year>
for the others; without -SkipBuild only that version is built). The default is 2027; the other
versions get the baseline before a major release (above). Every start of c4dpy gets a preferences folder of its own,
one per version (%TEMP%\cyclesrender_c4d_tests\prefs_c4dpy_<year>), and the window tests a fresh
one each; a new preferences folder gets the licence method "Maxon App" (what Cinema 4D stores when
it is chosen), as c4dpy otherwise asks for it. Plugins installed into the Cinema 4D folder itself
(<installation>\plugins), other than Redshift, are left out of every start (g_moduleNegatives):
they are not under test, and one of them ended c4dpy at start on this machine. A Cinema 4D without
its Python runtime (resource\modules\python\libs) cannot run the tests at all, which is said and
counted as failed.

-Changed takes the files that differ from the last commit (git diff against HEAD, staged or
not, and new files git does not ignore) and, because tests\ is not in git, the files in tests\
changed since the last commit was made. It looks each one up in the map below ($c4dTests and
$bridgeTests, field Covers; $coreFiles; $noTests), prints which tests it chose for which file,
and runs them together with the always-run set ($alwaysRun). A file the map does not know
runs the core set ($coreSet) and is named as unmapped: add it to the map then. A change of
documents only runs nothing. -Only adds tests to what -Changed chose; without -Changed it is
the whole choice. When the sprint's full run finds a fault that a selected run missed, connect
the file to the test here in the same piece of work.

Bridge tests need only Blender. Cinema 4D tests run in c4dpy (no window) with the plugin from
build\plugin and the bridge from the repository. Tests that need the same start settings
(the g_cyclesRender... settings Cinema 4D reads once at start) share one start of c4dpy and
run one after another in a fixed order, which is printed; each starts from a known state
(tests\c4d\lib\crrun.py) and has its own result and time. A capture or dump file is one per
start and is emptied before every script. When c4dpy ends before every test of a start has
reported, the test that was running fails and the ones not yet run are run again, each alone.
The last tests, ipr_window_checks, ipr_follow_window, ipr_lights_window, ipr_navigate_window,
ipr_live_window, ipr_optimized_window, ipr_values_window, ipr_threads_window, ipr_frames_window, new_light_window, baked_files_window,
blender_missing_window and default_engine_window,
need windows and start the
regular Cinema 4D for about half a minute each, with a preferences folder of their own so that the
user's preferences and layout stay untouched (and, field PrefsFiles, files put into it before the start).
ipr_navigate_window posts mouse and key messages to that Cinema 4D's IPR window from a process of its
own (tests\c4d\lib\post_input.ps1); the real mouse pointer is not moved.

Details of a failed test: its JSON result is %TEMP%\cyclesrender_c4d_tests\<name>.result.json,
the output of its start <start>.txt next to it (the start is printed with each group), and it
can be run alone with -Only <name> or tests\c4d\run_c4d_test.ps1 (the command is in the test's
first comment). Preview pictures of the comparison tests are written to
%TEMP%\cyclesrender_c4d_tests\images.
#>
param(
  [switch]$SkipBuild,
  [string[]]$Only = @(),
  [switch]$Changed,
  [string[]]$Files = @(),
  [switch]$DryRun,
  [switch]$Alone,
  [string]$Blender = "5.2",
  [string]$C4D = "2027"
)

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repo
# "5.3" stands for the Blender of that version in the installer's usual place.
if ($Blender -match '^\d+\.\d+$') { $Blender = "C:\Program Files\Blender Foundation\Blender $Blender\blender.exe" }
if (-not (Test-Path $Blender)) { Write-Output "-Blender: there is no $Blender"; exit 1 }
# "2026" stands for the Cinema 4D of that year in the installer's usual place; the year of an
# installation named by its folder is read from its executable.
if ($C4D -match '^\d{4}$') { $C4D = "C:\Program Files\Maxon Cinema 4D $C4D" }
$c4dExe = Join-Path $C4D "Cinema 4D.exe"
$c4dPy = Join-Path $C4D "c4dpy.exe"
if (-not (Test-Path $c4dPy) -or -not (Test-Path $c4dExe)) { Write-Output "-C4D: there is no Cinema 4D with c4dpy at $C4D"; exit 1 }
$c4dYear = [string](Get-Item $c4dExe).VersionInfo.FileMajorPart
if (@("2024", "2025", "2026", "2027") -notcontains $c4dYear) { Write-Output "-C4D: Cinema 4D $c4dYear is not supported (2024 to 2027)."; exit 1 }
$c4dVersion = "Cinema 4D " + (Get-Item $c4dExe).VersionInfo.ProductVersion
# Without Python inside Cinema 4D no test script can run (c4dpy ends at start, the harness never loads).
$c4dPython = Test-Path (Join-Path $C4D "resource\modules\python\libs")
if (-not (Test-Path "tests\c4d\run_c4d_test.ps1")) {
  Write-Output "The tests folder is not here: the tests are kept locally and are not part of the repository (docs\development.md, 'Tests')."
  exit 1
}
$out = Join-Path $env:TEMP "cyclesrender_c4d_tests"
New-Item -ItemType Directory -Force $out | Out-Null
$plugins = if ($c4dYear -eq "2027") { "build\plugin\bin\Release\plugins" } else { "build\plugin-$c4dYear\bin\Release\plugins" }
# The licence method "Maxon App", as Cinema 4D writes it into licensetype.prf of a preferences folder
# when it is chosen; without it c4dpy asks on its console. It holds no account data.
$licenceMethod = [byte[]](("ff 4d 41 58 4f 4e 1f 6e 65 74 2e 6d 61 78 6f 6e 2e 66 69 6c 65 69 64 2e 6c 69 63 65 6e 73 65 74 79 70 65 2e 76 32 76 01 02 ff 00 06 " +
  "75 69 6e 74 33 32 00 00 06 00 00 00") -split " " | ForEach-Object { [Convert]::ToByte($_, 16) })
function New-PrefsFolder($folder, [switch]$Fresh) {
  if ($Fresh -and (Test-Path $folder)) { Remove-Item $folder -Recurse -Force }
  New-Item -ItemType Directory -Force $folder | Out-Null
  $file = Join-Path $folder "licensetype.prf"
  if (-not (Test-Path $file)) { [IO.File]::WriteAllBytes($file, $licenceMethod) }
}
# Plugins installed into the Cinema 4D folder itself, Redshift excepted: left out of every start.
$foreignModules = @(Get-ChildItem (Join-Path $C4D "plugins") -Recurse -Filter *.xdl64 -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -notmatch '\\redshift\\' } | ForEach-Object { $_.BaseName } | Sort-Object -Unique)
$results = @()
$runWatch = [Diagnostics.Stopwatch]::StartNew()

# --- the tests and the map ----------------------------------------------------------------------
# Covers: the paths (wildcards, from the repository root) whose change this test checks. A test's
# own script is always covered. P and B are short for the plugin's source and the bridge.
$P = "plugin/cyclesrender/source/"
$R = "plugin/cyclesrender/res/"
$B = "bridge/c4d_bridge/"

# Bridge alone, no Cinema 4D.
$bridgeTests = @(
  @{ Name = "test_protocol"; Covers = @("${B}protocol.py", "${B}server.py", "${B}main.py") },
  @{ Name = "test_render"; Covers = @("${B}final/__init__.py", "${B}scene/meshes.py", "${B}scene/cameras.py", "${B}scene/lights.py") },
  @{ Name = "test_scene"; Covers = @("${B}scene/*", "${B}render/*") },
  @{ Name = "test_geometry"; Covers = @("${B}scene/meshes.py", "${B}scene/instancers.py", "${B}scene/objects.py", "${B}scene/pointclouds.py", "${B}scene/curves.py", "${B}scene/materials.py", "${B}scene/common.py") },
  @{ Name = "test_viewport"; Covers = @("${B}viewport/*") },
  @{ Name = "test_native"; OnHold = "the Cycles Native Material is on hold"; Covers = @("${B}scene/native.py") },
  @{ Name = "test_preview"; Covers = @("${B}final/preview.py") },
  @{ Name = "test_cameras"; Covers = @("${B}scene/cameras.py") },
  @{ Name = "test_output"; Covers = @("${B}final/__init__.py", "${B}scene/objects.py", "${B}render/settings.py") },
  @{ Name = "measure_lights"; Covers = @("${B}scene/lights.py") },
  @{ Name = "test_lights"; Covers = @("${B}scene/lights.py") },
  @{ Name = "test_devices"; Covers = @("${B}render/devices.py", "${B}render/settings.py") },
  # Frames published at most about 30 a second, progress still right; the device reported.
  @{ Name = "test_frame_rate"; Covers = @("${B}viewport/*", "${B}render/devices.py", "${B}render/settings.py", "tests/bridge/frames.py") },
  @{ Name = "test_versions"; Covers = @("${B}render/settings.py", "${B}scene/materials.py", "${P}videopost/render_settings_table.h", "tools/make_settings.py",
                                        "tools/make_nodes.py") },
  # The `proxy` item and `blend_info` (0.3.0, F7): a .blend linked, instanced, moved, removed, linked anew,
  # the scene cleared, in a background and in a windowed Blender; no library left behind.
  @{ Name = "test_proxy"; Covers = @("${B}scene/proxies.py", "${B}scene/__init__.py", "${B}server.py", "tests/bridge/make_proxy_blend.py") },
  # The `compositor` item (0.3.0, F10): effects on and off, opacity, two effects in both orders, Blender's
  # presets, the passes and the picture before the compositor, values set without another tree, removal and
  # nothing left behind, in final frames and in the viewport (which must agree), Cycles and Eevee.
  @{ Name = "test_compositor"; Covers = @("${B}scene/compositor.py", "${B}scene/__init__.py", "${B}scene/materials.py", "${B}server.py", "${B}final/__init__.py",
                                          "${B}viewport/__init__.py") },
  # The file Blender writes itself (0.3.0, F6): exactly the passes asked for, Cryptomatte with its
  # manifest and the items' names, half and full float, the picture unchanged by it, the composite in
  # the file, the path taken literally, nothing left behind; Cycles and Eevee.
  @{ Name = "test_files"; Covers = @("${B}final/files.py", "${B}final/__init__.py", "${B}scene/common.py", "${B}scene/objects.py", "${B}scene/instancers.py",
                                     "${B}scene/materials.py", "${B}render/settings.py", "tests/bridge/exr_info.py") },
  # A node that names an object (0.3.0, F12): the `empty` item and a node's `object`, whatever the order of the items,
  # the empty moved alone, removed, a name not held; Cycles and Eevee.
  @{ Name = "test_node_objects"; Covers = @("${B}scene/objects.py", "${B}scene/materials.py", "${B}scene/__init__.py", "${B}scene/world.py", "${B}scene/common.py") }
)

# Plugin inside Cinema 4D (c4dpy). Settings: for the start ("<file>" is a file of the start, emptied
# before each test); ExpectLog: the verdict is a line of the log; GoneAfter: the session's folder
# of baked textures, named in the result, must be gone once Cinema 4D has quit; Window: runs in the
# regular Cinema 4D, which opens and closes on screen; CleanPrefs: with a preferences folder of its own.
$c4dTests = @(
  @{ Name = "session_selftest"; Settings = @{ g_cyclesRenderSelfTest = "4" }; ExpectLog = "Cycles Render self-test: PASSED all"
     Covers = @("${P}session/*", "${P}platform/socket*", "${P}platform/shared_memory*", "${P}platform/environment*") },
  @{ Name = "traversal"; Settings = @{ g_cyclesRenderDumpFile = "<file>" }
     Covers = @("${P}scene/scene_exporter.*", "${P}scene/scene_dump.*", "${P}scene/mesh_export.*") },
  @{ Name = "coords"; Covers = @("${P}scene/coords.*", "${P}scene/camera_export.*") },
  @{ Name = "meshes"; Covers = @("${P}scene/mesh_export.*", "${B}scene/meshes.py") },
  @{ Name = "cameras"; Covers = @("${P}scene/camera_export.*", "${B}scene/cameras.py") },
  @{ Name = "lights"; Covers = @("${P}scene/light_export.*", "${B}scene/lights.py", "${P}scene/document_colors.*") },
  @{ Name = "materials"; Covers = @("${P}materials/material_export.*", "${P}materials/shader_translate.*", "${P}materials/node_graph.*", "${B}scene/materials.py", "${P}scene/document_colors.*") },
  @{ Name = "sky_orientation"; Covers = @("${P}scene/environment_export.*", "${B}scene/world.py", "${P}scene/document_colors.*") },
  @{ Name = "settings"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}videopost/render_settings_*", "${R}description/vpcyclesrender.*", "${R}strings_en-US/description/vpcyclesrender.str", "${B}render/*", "${B}scene/world.py", "tools/make_settings.py") },
  @{ Name = "sequence"; Covers = @("${P}videopost/cycles_videopost.*", "${P}scene/scene_writer.*") },
  @{ Name = "render_cancel"; Covers = @("${P}videopost/cycles_videopost.*", "${P}session/bridge_client.*", "${B}final/__init__.py") },
  @{ Name = "golden"; Covers = @("tests/scenes/*", "tests/golden/*", "${P}scene/*", "${P}materials/material_export.*", "${P}materials/shader_translate.*") },
  @{ Name = "shaders"; Covers = @("${P}materials/shader_translate.*", "${P}materials/baked_files.*", "${P}materials/material_export.*", "${P}scene/document_colors.*") },
  @{ Name = "mapping"; Covers = @("${P}materials/material_export.*", "${P}materials/shader_translate.*", "${P}scene/mesh_export.*") },
  @{ Name = "node_materials"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}materials/exchange_material.*", "${P}materials/redshift_material.*", "${P}materials/redshift_nodes.*", "${P}materials/node_graph.*", "${B}scene/materials.py") },
  @{ Name = "geometry"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/mesh_export.*", "${P}scene/scene_exporter.*", "${P}materials/shader_translate.*", "${P}scene/document_colors.*", "${B}scene/instancers.py", "${B}scene/meshes.py") },
  @{ Name = "motion_blur"; Covers = @("${P}scene/mesh_export.*", "${P}scene/camera_export.*", "${P}videopost/*", "${B}render/settings.py", "${B}scene/objects.py") },
  @{ Name = "passes"; Covers = @("${P}scene/light_export.*", "${P}videopost/*","${B}final/__init__.py", "${B}render/settings.py", "${B}scene/objects.py", "tools/make_settings.py", "${R}description/vpcyclesrender.*") },
  @{ Name = "region"; Covers = @("${P}videopost/cycles_videopost.*", "${B}final/__init__.py") },
  # The Output tab's "Files Written by Blender" (0.3.0, F6): the settings, a still from a saved document
  # (tokens, a relative path, Cryptomatte named as in Cinema 4D, the passes chosen for the file, the
  # picture unchanged), no file for other renders, an animation through Commandline.exe named per frame.
  @{ Name = "file_output"; Settings = @{ g_cyclesRenderLogCalls = "true" }; Covers = @("${P}videopost/file_output.*", "${P}videopost/cycles_videopost.*", "${P}videopost/render_settings_map.*",
                                      "${P}scene/scene_exporter.*", "${P}materials/material_export.*", "${R}description/vpcyclesrender.*",
                                      "${R}strings_en-US/description/vpcyclesrender.str", "tools/make_settings.py", "${B}final/*", "tests/bridge/exr_info.py") },
  @{ Name = "volumes"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/volume_export.*", "${B}scene/volumes.py", "${P}tags/object_tag.*") },
  @{ Name = "hair_particles"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/curves_export.*", "${B}scene/curves.py", "${B}scene/pointclouds.py", "${P}scene/document_colors.*") },
  # Hair (0.3.0, F2): the hair material's shape against the hairs Cinema 4D generates, a texture tag's material
  # for the colour, and which changes make the IPR generate a hair again.
  @{ Name = "hair"; Settings = @{ g_cyclesRenderCapture = "<file>"; g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/curves_export.*", "${P}scene/scene_writer.*", "${B}scene/curves.py") },
  @{ Name = "visibility"; Covers = @("${P}scene/light_export.*", "${P}scene/scene_exporter.*", "${B}scene/lights.py", "${B}scene/objects.py") },
  @{ Name = "redshift"; Covers = @("${P}scene/light_export.*", "${P}scene/camera_export.*", "${B}scene/lights.py", "${B}scene/cameras.py") },
  @{ Name = "object_tag"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}tags/object_tag.*", "${R}description/tcyclesobject.*", "${R}strings_en-US/description/tcyclesobject.str", "${P}scene/light_export.*", "${P}scene/curves_export.*", "${B}scene/instancers.py", "${B}scene/objects.py") },
  @{ Name = "fire_vdb"; Covers = @("${P}scene/volume_export.*", "${B}scene/volumes.py", "${P}tags/object_tag.*") },
  @{ Name = "redshift_material"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}materials/redshift_material.*", "${P}materials/redshift_nodes.*", "${P}materials/node_graph.*", "${P}ipr/ipr_controller.*") },
  @{ Name = "redshift_nodes"; Covers = @("${P}materials/redshift_nodes.*", "${P}materials/redshift_scene_data.*", "${P}materials/redshift_material.*", "${P}materials/node_graph.*", "${P}scene/coords.*", "${B}scene/materials.py") },
  @{ Name = "redshift_lookalikes"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}materials/redshift_lookalikes.*", "${P}materials/redshift_nodes.*", "${P}materials/redshift_material.*", "${P}materials/material_export.*",
                "${P}scene/scene_exporter.*", "${B}scene/objects.py", "${B}scene/instancers.py", "${B}scene/materials.py", "${P}scene/redshift_ids.h") },
  # Data of the scene a material reads by name (0.3.0, F12 and F13): per particle on a point cloud and on instances, per
  # MoGraph clone, an object's user data, vertex maps and vertex colours, through Redshift's user data and attribute
  # nodes and a Cycles graph's Attribute node, each next to Redshift's own render.
  @{ Name = "scene_data"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/element_data.*", "${P}materials/redshift_scene_data.*", "${P}materials/redshift_nodes.*", "${P}materials/redshift_lookalikes.*",
                "${P}materials/material_export.*", "${P}materials/cycles_node_material.*", "${P}scene/curves_export.*", "${P}scene/scene_exporter.*",
                "${B}scene/pointclouds.py", "${B}scene/instancers.py", "${B}scene/objects.py", "${B}scene/common.py") },
  @{ Name = "redshift_dome"; Covers = @("${P}scene/environment_export.*", "${P}scene/light_export.*", "${P}scene/redshift_ids.h", "${B}scene/world.py") },
  # Every Redshift light case against Redshift's own render (units, shapes, scale, spread,
  # decay, temperature in two render spaces), Eevee beside it, the IPR's light-only update, "Take Values".
  @{ Name = "redshift_lights"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/light_export.*", "${P}scene/redshift_ids.h", "${P}scene/document_colors.*", "${P}lights/light_tag.*", "${P}lights/light_settings.*", "${B}scene/lights.py") },
  # Objects as sources (0.3.0, F12): the Texture Coordinate node's link to an object of the scene, its axes in the picture
  # (a null, moved, turned, through its parent, a camera, a hidden cube, named twice, deleted, saved and loaded, Eevee),
  # nothing more sent without one, and the IPR sending the `empty` alone while the object moves, Cycles and Eevee.
  @{ Name = "node_objects"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}nodespace/object_link.*", "${P}nodespace/cycles_nodespace.*", "${P}nodespace/cycles_nodes_table.h", "${P}materials/cycles_node_material.*",
                "${P}materials/material_export.*", "${P}scene/scene_exporter.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "${R}nodes/*",
                "tools/make_nodes.py", "${B}scene/objects.py", "${B}scene/materials.py", "${B}scene/__init__.py") },
  @{ Name = "cycles_nodes"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}nodespace/*", "${P}materials/cycles_*", "${R}nodes/*", "tools/make_nodes.py", "tools/dump_blender_nodes.py", "${B}scene/materials.py", "${P}ipr/ipr_testhook.*") },
  @{ Name = "native_material"; OnHold = "the Cycles Native Material is on hold"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderNativeMaterial = "true" }
     Covers = @("${P}materials/native_material.*", "${B}scene/native.py", "${R}description/mcyclesnative.*", "${R}strings_en-US/description/mcyclesnative.str", "${R}native_default.preview", "tools/make_native_placeholder.py") },
  @{ Name = "camera_tag"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}tags/camera_tag.*", "${R}description/tcyclescamera.*", "${R}strings_en-US/description/tcyclescamera.str", "${P}scene/camera_export.*",
                "${B}scene/cameras.py", "${P}ipr/ipr_controller.*", "${R}icons/*", "tools/make_icons.py") },
  # The Imager tab of the Cycles Camera tag (0.3.0, F10): the layers set as the Attribute Manager sets them,
  # the item sent, the picture with and without effects, opacity, order, Eevee, an effect the Blender in use
  # lacks, another camera, saving, copying, undo, and the IPR (its picture the final frame's, a value alone).
  @{ Name = "imager"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}tags/imager_*", "${P}tags/camera_tag.*", "${P}scene/imager_export.*", "${P}materials/node_graph.*", "${R}description/tcyclescamera.*",
                "${R}strings_en-US/description/tcyclescamera.str", "${P}ipr/ipr_controller.*", "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}sdk_compat.h",
                "${B}scene/compositor.py", "${B}final/__init__.py",
                "${B}viewport/__init__.py", "${B}server.py", "tools/make_effects.py", "tools/dump_blender_effects.py") },
  @{ Name = "ipr"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_worker.*", "${P}ipr/ipr_testhook.*", "${P}ipr/ipr_trace.*", "${P}platform/process_stats*", "${B}viewport/*") },
  @{ Name = "ipr_reuse"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/scene_writer.*", "${P}scene/scene_exporter.*", "${P}scene/mesh_export.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*",
                "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}session/protocol.*", "${B}protocol.py", "${B}scene/__init__.py", "${P}materials/material_export.*") },
  @{ Name = "ipr_lights"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/light_export.*",
                "${P}lights/*", "${B}scene/lights.py") },
  # What a step of a change in progress sends alone and what it exports whole, the gate
  # on redraws, the settle export, a heavy generator judged without a pass over its data. It runs the
  # kinds of change of ipr_reuse.py as well (it reads that script).
  @{ Name = "ipr_optimized"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/mesh_export.*",
                "${P}scene/light_export.*", "${P}scene/camera_export.*", "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*",
                "${R}strings_en-US/description/prefscyclesrender.str", "${B}scene/objects.py", "${B}scene/__init__.py", "tests/c4d/ipr_reuse.py",
                "${P}materials/material_export.*") },
  # The manual mode (Preferences, "Refresh Whole Scene Only on Reload"): steps only, never a whole export
  # by itself; what cannot go alone waits for Reload, which sends what differs.
  @{ Name = "ipr_manual"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*",
                "${R}strings_en-US/description/prefscyclesrender.str") },
  # The values that follow inside an evaluation of the document (an Attribute Manager drag): a
  # camera's, the parameters of primitives and generators, deformers; single objects collected again and
  # their meshes sent alone, what still waits for the end, the guard after a heavy mesh.
  @{ Name = "ipr_values"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/mesh_export.*",
                "${P}scene/light_export.*", "${P}scene/camera_export.*", "${P}tags/camera_tag.*", "${P}materials/material_export.*",
                "${B}scene/objects.py", "${B}scene/instancers.py", "${B}scene/meshes.py", "${B}scene/__init__.py") },
  @{ Name = "ipr_navigate"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}ipr/ipr_navigation.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "${P}scene/camera_export.*", "${P}scene/scene_exporter.*",
                "${P}tags/camera_tag.*", "${P}prefs/settings.*", "${B}viewport/*", "${B}scene/cameras.py") },
  @{ Name = "output"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}videopost/output_settings.*", "${P}videopost/render_settings_*", "${P}videopost/cycles_videopost.*", "${P}scene/camera_export.*", "${P}ipr/ipr_controller.*",
                "${R}description/vpcyclesrender.*", "${R}strings_en-US/description/vpcyclesrender.str", "tools/make_settings.py", "${B}final/__init__.py") },
  @{ Name = "image_nodes"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}nodespace/path_commands.*", "${P}nodespace/cycles_nodespace.*", "${P}materials/cycles_node_material.*", "${P}materials/material_export.*",
                "${R}nodes/*", "tools/make_nodes.py", "${P}ipr/ipr_testhook.*") },
  @{ Name = "ipr_add"; Settings = @{ g_cyclesRenderIprTest = "true" }; Covers = @("${P}ipr/ipr_add.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_testhook.*", "${P}lights/light_tag.*", "${P}lights/light_object.*") },
  @{ Name = "cycles_lights"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}lights/*", "${R}description/ocycleslight.*", "${R}description/tcycleslight.*", "${R}strings_en-US/description/ocycleslight.str",
                "${R}strings_en-US/description/tcycleslight.str", "${P}scene/light_export.*", "${B}scene/lights.py", "${R}icons/*", "tools/make_icons.py") },
  # The Cycles Fog object (0.3.0, F5): Beer-Lambert, colours, emission, shape against a Cube of the
  # Standard renderer, the check, the IPR's move and value alone, the Add menu; the noise.
  @{ Name = "fog_object"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}objects/fog_object.*", "${P}scene/fog_export.*", "${R}description/ocyclesfog.*", "${R}strings_en-US/description/ocyclesfog.str",
                "${P}ipr/ipr_add.*", "${P}scene/volume_export.*", "${B}scene/volumes.py", "${B}scene/meshes.py", "${R}icons/*", "tools/make_icons.py") },
  # The Cycles Environment object (0.3.0, F4): colour, HDRI against the Sky in 22 directions, Blender's Sky
  # Texture and its sun, which environment is used, the world written alone in the IPR.
  @{ Name = "environment_object"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}objects/environment_object.*", "${P}scene/environment_export.*", "${R}description/ocyclesenvironment.*",
                "${R}strings_en-US/description/ocyclesenvironment.str", "${P}ipr/ipr_add.*", "${P}ipr/ipr_controller.*", "${B}scene/world.py",
                "${R}icons/*", "tools/make_icons.py", "${P}nodespace/cycles_nodes_table.h",
                "${P}materials/cycles_material.*", "${P}materials/cycles_node_material.*", "${P}nodespace/path_commands.*") },
  # The Cycles Scatter object (0.3.0, F8): the four distributions and their determinism, the restrictions,
  # Normal Align, MoGraph effectors and fields (through effectors and on the object), the copies rendered against
  # the Standard renderer (places; a Cloner's clone colour), a surface below it, the IPR, 100,000 copies.
  @{ Name = "scatter_object"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}objects/scatter_*", "${R}description/ocyclesscatter.*", "${R}strings_en-US/description/ocyclesscatter.str",
                "${P}scene/scene_exporter.*", "${P}ipr/ipr_add.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_testhook.*", "${B}scene/instancers.py",
                "${R}icons/*", "tools/make_icons.py") },
  # The Cycles Proxy object (0.3.0, F7): the file read by a Blender (the menu, the box, stale content), the
  # proxy's place against the same boxes built natively, two proxies of one file, a named collection, what is
  # not there, the frame offset, the file's lamp, the IPR (a move alone), a Cloner's and a Scatter's copies.
  @{ Name = "proxy_object"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}objects/proxy_object.*", "${P}scene/proxy_export.*", "${R}description/ocyclesproxy.*", "${R}strings_en-US/description/ocyclesproxy.str",
                "${P}scene/scene_exporter.*", "${P}ipr/ipr_add.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_testhook.*", "${P}nodespace/path_commands.*",
                "${B}scene/proxies.py", "${B}scene/__init__.py", "${B}server.py", "${R}icons/*", "tools/make_icons.py", "tests/bridge/make_proxy_blend.py") },
  @{ Name = "light_enable"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/light_export.*", "${P}scene/scene_exporter.*", "${P}lights/*", "${P}ipr/ipr_controller.*", "${B}scene/lights.py", "tests/scenes/old/*") },
  @{ Name = "light_shadows"; Covers = @("${P}scene/light_export.*", "${P}lights/*", "${B}scene/lights.py") },
  @{ Name = "editor_camera"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/camera_export.*", "${P}ipr/ipr_controller.*", "${B}scene/cameras.py", "${B}viewport/*") },
  @{ Name = "devices"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}prefs/devices.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}videopost/render_settings_map.*", "${B}render/devices.py", "${B}render/settings.py", "${P}ipr/ipr_testhook.*") },
  # The device really used (final frames and the IPR), the threads left free, the priority rule.
  @{ Name = "ipr_device"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}prefs/devices.*", "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*",
                "${R}strings_en-US/description/prefscyclesrender.str", "${P}videopost/render_settings_*", "${R}description/vpcyclesrender.*",
                "${R}strings_en-US/description/vpcyclesrender.str", "tools/make_settings.py", "${P}platform/process_stats*",
                "${P}ipr/ipr_controller.*", "${P}ipr/ipr_worker.*", "${P}ipr/ipr_testhook.*", "${B}render/*", "${B}viewport/*") },
  @{ Name = "default_engine"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}videopost/cycles_videopost.*", "${P}videopost/render_settings_map.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*", "tools/make_settings.py") },
  @{ Name = "ipr_region"; Settings = @{ g_cyclesRenderIprTest = "true" }; Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_worker.*", "${B}viewport/*") },
  @{ Name = "colour"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/document_colors.*", "${P}ipr/ipr_image_area.*", "${P}ipr/ipr_controller.*", "${P}materials/shader_translate.*", "${P}materials/material_export.*", "${B}render/settings.py") },
  @{ Name = "denoise"; Settings = @{ g_cyclesRenderIprTest = "true" }; Covers = @("${P}videopost/render_settings_*", "${P}ipr/ipr_controller.*", "${B}render/settings.py", "${B}viewport/*") },
  @{ Name = "baked_files"; Settings = @{ g_cyclesRenderIprTest = "true" }; GoneAfter = $true
     Covers = @("${P}materials/baked_files.*", "${P}platform/file_lock*", "${P}materials/shader_translate.*") },
  @{ Name = "ipr_window_checks"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/*", "${R}icons/*", "tools/make_icons.py", "${P}materials/material_preview.*", "${P}materials/baked_files.*", "${P}prefs/*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str", "tests/c4d/harness/*") },
  @{ Name = "ipr_follow_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_trace.*", "${P}ipr/ipr_image_area.*", "${P}ipr/ipr_scenehook.*", "${P}session/frame_watcher.*",
                "${B}viewport/*", "tests/c4d/harness/*") },
  @{ Name = "ipr_lights_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_trace.*", "${P}session/frame_watcher.*",
                "${P}scene/light_export.*", "${P}lights/*", "tests/c4d/harness/*") },
  @{ Name = "ipr_navigate_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_navigation.*", "${P}ipr/ipr_image_area.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_trace.*",
                "${P}ipr/ipr_scenehook.*", "${P}prefs/prefs_page.*", "${P}prefs/settings.*", "${R}description/prefscyclesrender.*",
                "${R}strings_en-US/description/prefscyclesrender.str", "${P}session/frame_watcher.*", "tests/c4d/lib/post_input.ps1", "tests/c4d/harness/*") },
  # The Preferences' live updates off and on in one running IPR: it sets them through the page, in
  # its own preferences folder.
  @{ Name = "ipr_live_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_trace.*", "${P}ipr/ipr_testhook.*",
                "${P}session/frame_watcher.*", "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*",
                "${R}strings_en-US/description/prefscyclesrender.str", "tests/c4d/harness/*") },
  # "Optimized Live Updates" on (the default) and off in one running IPR, with emulated
  # drags, view navigation and playback: what is sent during each and that Blender is in sync after.
  @{ Name = "ipr_optimized_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_trace.*", "${P}ipr/ipr_testhook.*",
                "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/mesh_export.*", "${P}session/frame_watcher.*", "${P}prefs/settings.*",
                "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${B}scene/objects.py", "${P}materials/material_export.*", "tests/c4d/harness/*") },
  # Emulated Attribute Manager drags of camera values, parametric objects and deformers (also
  # with the evaluation on Cinema 4D's drawing thread, as a real slider's), a deformer dragged as a tool
  # does, a heavy mesh and the guard; with "Optimized Live Updates" off, as before.
  @{ Name = "ipr_values_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_trace.*", "${P}ipr/ipr_testhook.*",
                "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/mesh_export.*", "${P}scene/camera_export.*",
                "${P}materials/material_export.*", "${P}session/frame_watcher.*", "${B}scene/objects.py", "${B}scene/instancers.py", "${B}scene/meshes.py",
                "tests/c4d/harness/*") },
  # The controller entered from two threads: during emulated drags with the evaluations on Cinema 4D's
  # drawing thread the IPR is stopped and the controller freed (timed into a step of that thread), the
  # camera it renders through is deleted or another one chosen, the document closed; Cinema 4D quits
  # with the IPR rendering.
  @{ Name = "ipr_threads_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_worker.*", "${P}ipr/ipr_trace.*",
                "${P}ipr/ipr_testhook.*", "${P}session/frame_watcher.*", "${P}scene/camera_export.*", "${P}main.cpp", "tests/c4d/harness/*") },
  # At most about 30 frames a second shown, the last one always; the status line's device.
  @{ Name = "ipr_frames_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_image_area.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_scenehook.*", "${P}ipr/ipr_trace.*",
                "${P}ipr/ipr_worker.*", "${P}ipr/ipr_testhook.*", "${P}session/frame_watcher.*", "${P}session/frame_reader.*",
                "${P}platform/process_stats*", "${B}viewport/*", "${B}render/*", "tests/bridge/frames.py", "tests/c4d/harness/*") },
  @{ Name = "new_light_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_add.*", "${P}lights/light_object.*", "tests/c4d/harness/*") },
  @{ Name = "baked_files_window"; Window = $true; CleanPrefs = $true; GoneAfter = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}materials/baked_files.*", "${P}materials/material_preview.*", "tests/c4d/harness/*") },
  # No g_cyclesRenderBlender: the Blender is the one the Preferences page holds; detection looks in an
  # empty folder (g_cyclesRenderBlenderSearch), so this machine's Blenders are not found. The supported
  # one it renders with at one point comes as CYCLESRENDER_TEST_BLENDER, which Cinema 4D does not read.
  @{ Name = "blender_missing_window"; Window = $true; CleanPrefs = $true
     Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlenderSearch = (Join-Path $out "no_blender_here"); CYCLESRENDER_TEST_BLENDER = $Blender }
     Covers = @("${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${P}session/blender_install.*", "${P}session/blender_process.*", "${P}ipr/ipr_controller.*",
                "${P}ipr/ipr_worker.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_image_area.*", "${P}materials/material_preview.*", "${P}nodespace/cycles_nodespace.*",
                "${P}videopost/cycles_videopost.*", "${B}main.py", "tests/c4d/harness/*") },
  # The user's template scene (new.c4d, saved by Cinema 4D 2024 so that every version reads it) is in
  # the preferences folder before the start: Cinema 4D reads it only then.
  @{ Name = "default_engine_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     PrefsFiles = @{ "new.c4d" = "tests\scenes\templates\new_cycles_render.c4d" }
     Covers = @("${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}videopost/cycles_videopost.*", "${P}videopost/render_settings_map.*", "${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*",
                "tests/scenes/templates/*", "tests/c4d/harness/*") }
)

# Everybody's files: a change in one of them runs the core set (and the tests that cover the file
# as well). The core set is enough because every other test reaches its own part through the same
# code these exercise: test_protocol and session_selftest the framing, the handshake and the start
# and end of Blender from both sides (session/, platform/, protocol.py, main.py); test_render and
# test_scene the bridge's request handlers and the final driver with the common builders
# (server.py, final/, scene/common.py, objects.py); test_viewport and ipr the second run loop,
# the frame block and the delta sync of the IPR (scene_writer's SyncState, frame_reader/watcher);
# traversal the scene walk that decides what renders (scene_exporter); coords, lights and
# materials a final frame through the videopost with the three things every scene has, placed
# geometry, a light and a material, against the Standard renderer (cycles_videopost, coords,
# parameters.h, material_export, prefs/settings: the Blender path); sequence one Blender for a
# sequence and only the changes sent; golden whole saved scenes against stored pictures. What it
# does not reach is left to the sprint's full run. sdk_compat.h and the build script hold what
# differs between the Cinema 4D versions: run the core set in each version the change concerns
# (-C4D), and the whole baseline in all of them at the end of the sprint.
$coreFiles = @(
  "${P}main.cpp", "${P}ids.h", "${P}version.h", "${P}sdk_compat.h","${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/parameters.h",
  "${P}scene/coords.*", "${P}session/*", "${P}platform/socket*", "${P}platform/shared_memory*", "${P}platform/environment*",
  "${P}prefs/settings.*", "${P}videopost/cycles_videopost.*", "${P}materials/material_export.*",
  "plugin/cyclesrender/project/*", "${R}c4d_symbols.h", "${R}strings_en-US/c4d_strings.str",
  "${B}main.py", "${B}server.py", "${B}protocol.py", "${B}errors.py", "${B}__init__.py", "${B}scene/__init__.py", "${B}scene/common.py",
  "${B}scene/objects.py", "${B}final/__init__.py", "${B}render/__init__.py", "${B}viewport/__init__.py",
  "tools/build_plugin.ps1", "tests/c4d/lib/*", "tests/c4d/c4dpy_runner.py", "tests/c4d/run_c4d_test.ps1", "tests/c4d/harness/*",
  "tests/bridge/client.py", "tests/bridge/scenes.py", "tests/bridge/frames.py"
)
$coreSet = @("test_protocol", "test_render", "test_scene", "test_viewport", "session_selftest", "traversal", "coords", "lights", "materials", "sequence", "golden", "ipr")
# The smoke test every run of -Changed includes (about a minute): the protocol, the session, one
# render against the Standard renderer.
$alwaysRun = @("test_protocol", "session_selftest", "coords")
# Files the map knows and that need no test of their own: what they are, and why. Smoke: the
# always-run set is enough for it; otherwise nothing runs for it.
$noTests = @(
  @{ Pattern = "*.md"; Why = "a document" }, @{ Pattern = "docs/*"; Why = "a document" }, @{ Pattern = "internal/*"; Why = "working notes" },
  @{ Pattern = "LICENSE"; Why = "a document" }, @{ Pattern = "bridge/LICENSE"; Why = "a document" }, @{ Pattern = "examples/*"; Why = "an example scene for users; no test opens it" },
  @{ Pattern = ".gitignore"; Why = "not code" }, @{ Pattern = "references/*"; Why = "read-only reference material" },
  @{ Pattern = "tools/make_release.ps1"; Why = "the release script checks its packages itself" },
  @{ Pattern = "tools/make_example_scene.py"; Why = "makes the example scene; no test opens it" },
  @{ Pattern = "tools/run_tests.ps1"; Why = "this script: the always-run set exercises it"; Smoke = $true },
  @{ Pattern = "bridge/__init__.py"; Why = "the add-on wrapper of an installed bridge; tests use the repository's main.py" },
  @{ Pattern = "bridge/blender_manifest.toml"; Why = "used only when the bridge is installed; make_release.ps1 checks it" },
  @{ Pattern = "tests/c4d/show_*.py"; Why = "a script for screenshots by hand, not in the baseline" },
  @{ Pattern = "tests/c4d/noop.py"; Why = "a script run by hand, not in the baseline" }, @{ Pattern = "tests/c4d/check_plugins.py"; Why = "a script run by hand, not in the baseline" },
  @{ Pattern = "tests/c4d/prefs_page.py"; Why = "a script run by hand, not in the baseline" }, @{ Pattern = "tests/c4d/ipr_window.py"; Why = "a script run by hand, not in the baseline" },
  @{ Pattern = "tests/c4d/viewport_render.py"; Why = "a script run by hand, not in the baseline" }, @{ Pattern = "tests/c4d/pyro.py"; Why = "not in the baseline (failing, see its first comment)" },
  @{ Pattern = "tests/c4d/measure_c4d_lights.py"; Why = "a measurement run by hand, not in the baseline" },
  @{ Pattern = "tests/README*"; Why = "a document" }
)

function Test-Like($file, $patterns) { foreach ($pattern in $patterns) { if ($file -like $pattern) { return $true } }; return $false }

# Tests on hold (field OnHold, the reason) are in no run: not the baseline, not -Changed, not -Only.
# A changed file that only they cover runs nothing and says so. To take one back in, remove the field.
$onHold = @($bridgeTests + $c4dTests | Where-Object { $_.OnHold })
$bridgeTests = @($bridgeTests | Where-Object { -not $_.OnHold })
$c4dTests = @($c4dTests | Where-Object { -not $_.OnHold })
if ($onHold.Count) { Write-Output ("on hold, not run: {0}" -f (($onHold | ForEach-Object { "$($_.Name) ($($_.OnHold))" }) -join ", ")) }

$allTests = @($bridgeTests | ForEach-Object { $_.Name }) + @($c4dTests | ForEach-Object { $_.Name })
foreach ($name in $Only) {
  $held = @($onHold | Where-Object { $_.Name -eq $name })
  if ($held.Count) { Write-Output "-Only: the test '$name' is on hold ($($held[0].OnHold)). Remove its OnHold field in tools\run_tests.ps1 to run it."; exit 1 }
  if ($allTests -notcontains $name) { Write-Output "-Only: there is no test '$name'. Tests: $($allTests -join ', ')"; exit 1 }
}

# --- -Changed: choose the tests from what differs from the last commit ------------------------------
$chosen = @()
if ($Changed) {
  if ($Files.Count) {
    $changedFiles = @($Files | ForEach-Object { $_.Replace("\", "/") })
    Write-Output ("-Changed -Files: as if {0} file(s) had changed" -f $changedFiles.Count)
  }
  else {
    $changedFiles = @(git diff --name-only HEAD) + @(git ls-files --others --exclude-standard)
    # tests\ is not in git: its files count as changed when written after the last commit was made.
    $committed = [DateTimeOffset]::Parse((git log -1 --format=%cI)).LocalDateTime
    $changedFiles += @(Get-ChildItem tests -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.LastWriteTime -gt $committed -and $_.FullName -notmatch "\\__pycache__\\" } |
      ForEach-Object { $_.FullName.Substring("$repo".Length + 1).Replace("\", "/") })
    $changedFiles = @($changedFiles | Where-Object { $_ } | Sort-Object -Unique)
    Write-Output ("-Changed: {0} file(s) differ from the last commit ({1})" -f $changedFiles.Count, (git log -1 --format="%h %s"))
  }
  $needsTests = $false
  $coreUsed = $false
  foreach ($file in $changedFiles) {
    $why = @()
    $own = [IO.Path]::GetFileNameWithoutExtension($file)
    $isTestScript = ($file -like "tests/c4d/*.py" -or $file -like "tests/bridge/*.py") -and $file -notlike "tests/*/*/*"
    $tests = @()
    if ($isTestScript -and $allTests -contains $own) { $tests += $own; $why += "the test itself" }
    $covering = @($bridgeTests + $c4dTests | Where-Object { Test-Like $file $_.Covers } | ForEach-Object { $_.Name })
    if ($covering.Count) { $tests += $covering; $why += "covered by " + ($covering -join ", ") }
    if (Test-Like $file $coreFiles) { $tests += $coreSet; $why += "everybody's file: the core set"; $coreUsed = $true }
    $held = @($onHold | Where-Object { (Test-Like $file $_.Covers) -or ($isTestScript -and $_.Name -eq $own) } | Select-Object -First 1)
    if (-not $tests.Count -and $held.Count) {
      Write-Output ("  {0}: nothing to run (its test {1} is on hold: {2})" -f $file, $held[0].Name, $held[0].OnHold)
      continue
    }
    $known = @($noTests | Where-Object { $file -like $_.Pattern } | Select-Object -First 1)
    if (-not $tests.Count -and $known.Count) {
      if ($known[0].Smoke) { $needsTests = $true; Write-Output ("  {0}: the always-run set ({1})" -f $file, $known[0].Why) }
      else { Write-Output ("  {0}: nothing to run ({1})" -f $file, $known[0].Why) }
      continue
    }
    $needsTests = $true
    if (-not $tests.Count) {
      $tests = $coreSet; $coreUsed = $true
      Write-Output ("  {0}: NOT IN THE MAP; runs the core set. Add it to the map in tools\run_tests.ps1" -f $file)
    }
    else {
      $tests = @($tests | Select-Object -Unique)
      Write-Output ("  {0}: {1}" -f $file, ($why -join "; "))
    }
    $chosen += $tests
  }
  if ($coreUsed) { Write-Output ("  the core set: {0}" -f ($coreSet -join ", ")) }
  if ($needsTests) {
    Write-Output ("  always run: {0}" -f ($alwaysRun -join ", "))
    $chosen += $alwaysRun
  }
  $chosen += $Only
  $chosen = @($allTests | Where-Object { $chosen -contains $_ })   # in the order of the baseline
  if (-not $chosen.Count) {
    Write-Output "Nothing to run: no change that a test checks."
    exit 0
  }
  Write-Output ("chosen: {0} test(s): {1}" -f $chosen.Count, ($chosen -join ", "))
}
elseif ($Only.Count) { $chosen = $Only }

function Wanted($name) { return ($chosen.Count -eq 0 -and -not $Changed) -or ($chosen -contains $name) }

# --- the starts of Cinema 4D --------------------------------------------------------------------------
# Tests with the same start settings share a start, in the order of the list; a test whose verdict
# needs the end of the session (GoneAfter) comes last in its start. Window tests start alone.
$groups = [ordered]@{}
foreach ($test in $c4dTests) {
  if (-not (Wanted $test.Name)) { continue }
  $settings = if ($test.Settings) { $test.Settings } else { @{} }
  $label = @($settings.Keys | Sort-Object | ForEach-Object { "$($_)=$($settings[$_])" }) -join " "
  $key = if ($test.Window -or $Alone) { "alone:" + $test.Name } else { "start:" + $label + "|" + $test.ExpectLog + "|" + [bool]$test.CleanPrefs }
  if (-not $groups.Contains($key)) { $groups[$key] = @() }
  $groups[$key] += $test
}
$starts = @()
foreach ($key in $groups.Keys) {
  $members = @($groups[$key] | Where-Object { -not $_.GoneAfter }) + @($groups[$key] | Where-Object { $_.GoneAfter })
  $first = $members[0]
  $settings = if ($first.Settings) { $first.Settings } else { @{} }
  $words = @()
  foreach ($pair in @(("g_cyclesRenderIprTest", "ipr"), ("g_cyclesRenderCapture", "capture"), ("g_cyclesRenderNativeMaterial", "native"),
                      ("g_cyclesRenderDumpFile", "dump"), ("g_cyclesRenderSelfTest", "selftest"))) {
    if ($settings.ContainsKey($pair[0])) { $words += $pair[1] }
  }
  $name = if ($members.Count -eq 1) { $first.Name } else { "start_" + $(if ($words.Count) { $words -join "_" } else { "plain" }) }
  $starts += [pscustomobject]@{ Name = $name; Tests = $members; Settings = $settings }
}

if ($DryRun) {
  if ($Changed -or $Only.Count) { Write-Output ("bridge tests: {0}" -f (@($bridgeTests | Where-Object { Wanted $_.Name } | ForEach-Object { $_.Name }) -join ", ")) }
  foreach ($start in $starts) {
    $shown = @($start.Settings.Keys | Sort-Object | ForEach-Object { "$($_)=$($start.Settings[$_])" }) -join " "
    Write-Output ("start '{0}' ({1}): {2}" -f $start.Name, $(if ($shown) { $shown } else { "no settings" }), (($start.Tests | ForEach-Object { $_.Name }) -join ", "))
  }
  exit 0
}

# The user may have a Blender of their own open; only processes that appear during the run count.
$blenderBefore = @(Get-Process blender -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$blenderVersion = (& $Blender --version 2>$null | Select-Object -First 1)
Write-Output ("Blender: {0} ({1})" -f $blenderVersion, $Blender)
Write-Output ("{0} ({1}), plugin from {2}" -f $c4dVersion, $C4D, $plugins)
if ($foreignModules.Count) { Write-Output ("left out of every start, installed into that Cinema 4D: {0}" -f ($foreignModules -join ", ")) }
if (-not $c4dPython) { Write-Output "$c4dVersion has no Python runtime ($C4D\resource\modules\python\libs is missing): c4dpy ends at start and the test harness cannot load, so no Cinema 4D test can run. Repair or reinstall that Cinema 4D." }

function Add-Result($name, $passed, $seconds, $note = "") {
  $script:results += [pscustomobject]@{ Test = $name; Result = $(if ($passed) { "passed" } else { "FAILED" }); Seconds = [int]$seconds }
  Write-Output ("{0,-22} {1} ({2:N0} s){3}" -f $name, $(if ($passed) { "passed" } else { "FAILED" }), $seconds, $note)
}

$buildSeconds = 0
if (-not $SkipBuild) {
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $PSScriptRoot "build_plugin.ps1") -Version $c4dYear *> (Join-Path $out "build.log")
  # The exit code alone is not trusted: a build that fell back on old project files can end with 0.
  $built = ($LASTEXITCODE -eq 0) -and (Test-Path "$plugins\cyclesrender\cyclesrender.xdl64") -and
    -not (Select-String -Path (Join-Path $out "build.log") -Pattern "error C\d+|error :|LNK\d+|CMake Error" -Quiet)
  $buildSeconds = $watch.Elapsed.TotalSeconds
  Add-Result "build ($c4dYear)" $built $buildSeconds
  if (-not $built) { Write-Output "see $out\build.log"; exit 1 }
}

foreach ($test in $bridgeTests) {
  if (-not (Wanted $test.Name)) { continue }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  python "tests\bridge\$($test.Name).py" $Blender *> (Join-Path $out "$($test.Name).txt")
  Add-Result $test.Name ($LASTEXITCODE -eq 0) $watch.Elapsed.TotalSeconds
}

foreach ($start in $starts) {
  if (-not $c4dPython) {
    foreach ($test in $start.Tests) { Add-Result $test.Name $false 0 " - not run: $c4dVersion has no Python runtime" }
    continue
  }
  $first = $start.Tests[0]
  $settings = $start.Settings.Clone()
  if ($foreignModules.Count) { $settings.g_moduleNegatives = $foreignModules -join ";" }
  foreach ($key in @($settings.Keys)) {
    if ($settings[$key] -ne "<file>") { continue }
    # A file of this start: gone before it begins (the runner also empties it before every test).
    $settings[$key] = Join-Path $out "$($start.Name)_$($key -replace '^g_cyclesRender', '').txt".ToLower()
    Remove-Item $settings[$key] -ErrorAction SilentlyContinue
  }
  if ($first.CleanPrefs) {
    $prefs = Join-Path $out "prefs_$($start.Name)"
    New-PrefsFolder $prefs -Fresh
    $settings.g_prefsPath = $prefs
    # Files a test needs in its preferences folder before Cinema 4D starts (PrefsFiles: name in the
    # folder = file of the repository), such as the user's template scene new.c4d, read only at start.
    if ($first.PrefsFiles) { foreach ($name in $first.PrefsFiles.Keys) { Copy-Item $first.PrefsFiles[$name] (Join-Path $prefs $name) } }
  }
  elseif (-not $first.Window) {
    # c4dpy: one preferences folder per version, kept between runs like c4dpy's own.
    $prefs = Join-Path $out "prefs_c4dpy_$c4dYear"
    New-PrefsFolder $prefs
    $settings.g_prefsPath = $prefs
  }
  if ($start.Tests.Count -gt 1) {
    $shown = @($start.Settings.Keys | Sort-Object | ForEach-Object { "$($_)=$($start.Settings[$_])" }) -join " "
    Write-Output ("-- one start of c4dpy ({0}), in this order: {1}" -f $(if ($shown) { $shown } else { "no settings" }), (($start.Tests | ForEach-Object { $_.Name }) -join ", "))
  }
  $summary = Join-Path $out "$($start.Name).summary.json"
  Remove-Item $summary -ErrorAction SilentlyContinue
  $arguments = @{ Headless = (-not $first.Window); Script = @($start.Tests | ForEach-Object { "tests\c4d\$($_.Name).py" }); ModulePath = $plugins
                  TimeoutSec = 600; Settings = $settings; Name = $start.Name; SummaryFile = $summary; Blender = $Blender; C4D = $c4dExe; C4DPy = $c4dPy }
  if ($first.ExpectLog) { $arguments.ExpectLog = $first.ExpectLog; $arguments.LogPattern = "Cycles Render self-test" }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & "tests\c4d\run_c4d_test.ps1" @arguments *> (Join-Path $out "$($start.Name).txt")
  $wall = $watch.Elapsed.TotalSeconds
  $verdicts = if (Test-Path $summary) { @((Get-Content $summary -Raw | ConvertFrom-Json).scripts) } else { @() }
  $scriptSeconds = 0.0
  foreach ($test in $start.Tests) {
    $verdict = @($verdicts | Where-Object { $_.name -eq $test.Name })
    # No verdict at all: the runner itself failed. That never counts as passed.
    $passed = $verdict.Count -eq 1 -and [bool]$verdict[0].passed
    $seconds = if ($verdict.Count -eq 1) { [double]$verdict[0].seconds } else { 0 }
    if ($start.Tests.Count -eq 1) { $seconds = $wall }   # alone: the whole start, as before
    $scriptSeconds += $seconds
    $note = ""
    if ($verdict.Count -eq 1 -and -not $verdict[0].passed) { $note = " - $($verdict[0].reason)" }
    if ($verdict.Count -eq 1 -and $verdict[0].start -ne $start.Name) { $note += " (run alone after its start ended early)" }
    if ($passed -and $test.GoneAfter) {
      $folder = (Get-Content (Join-Path $out "$($test.Name).result.json") -Raw | ConvertFrom-Json).notes.session_folder_to_be_gone_after_exit
      $passed = [bool]$folder -and -not (Test-Path $folder)
      if (-not $passed) { $note = " - the session folder is still there after the end: $folder"; Add-Content (Join-Path $out "$($start.Name).txt") "the session folder is still there after the end: $folder" }
    }
    Add-Result $test.Name $passed $seconds $note
  }
  if ($start.Tests.Count -gt 1) { Write-Output ("   start '{0}': {1:N0} s in all, {2:N0} s of them starting and quitting c4dpy and between tests; output in {0}.txt" -f $start.Name, $wall, ($wall - $scriptSeconds)) }
}

$left = @(Get-Process blender -ErrorAction SilentlyContinue | Where-Object { $blenderBefore -notcontains $_.Id }).Count
Write-Output "blender processes left running by the tests: $left"
$failed = @($results | Where-Object { $_.Result -ne "passed" })
Write-Output ("{0} of {1} passed ({2}, {3})" -f ($results.Count - $failed.Count), $results.Count, $blenderVersion, $c4dVersion)
Write-Output ("whole run {0:N0} s (build {1:N0} s)" -f $runWatch.Elapsed.TotalSeconds, $buildSeconds)
if ($failed.Count -gt 0 -or $left -gt 0) { exit 1 }
