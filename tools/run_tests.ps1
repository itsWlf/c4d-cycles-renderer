# SPDX-License-Identifier: Apache-2.0
<#
Runs the project's tests and prints one line per test: all of them (the baseline), the ones a
change needs (-Changed), or the ones named (-Only).

The tests themselves (the folder tests\) are kept locally and are not part of the repository;
without them this script has nothing to run. See docs\development.md, "Tests", for when to run
which: the tests of what changed for every piece of work, the whole baseline once per sprint,
before a commit the maintainer asks for, and before a release.

  .\tools\run_tests.ps1              build, bridge tests, then every Cinema 4D test (the baseline)
  .\tools\run_tests.ps1 -SkipBuild   the same without rebuilding the plugin
  .\tools\run_tests.ps1 -Only lights,materials
  .\tools\run_tests.ps1 -Changed     the tests for the files that differ from the last commit
  .\tools\run_tests.ps1 -Changed -DryRun   only print what -Changed would run, and why
  .\tools\run_tests.ps1 -Changed -Files bridge/c4d_bridge/scene/lights.py   as if only that file had changed
  .\tools\run_tests.ps1 -Alone       every Cinema 4D test in a start of its own (the old way)

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
The last three tests, ipr_window_checks, ipr_follow_window and baked_files_window, need windows
and start the regular Cinema 4D for about half a minute each, with a preferences folder of their
own so that the user's preferences and layout stay untouched.

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
  [string]$Blender = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
)

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repo
if (-not (Test-Path "tests\c4d\run_c4d_test.ps1")) {
  Write-Output "The tests folder is not here: the tests are kept locally and are not part of the repository (docs\development.md, 'Tests')."
  exit 1
}
$out = Join-Path $env:TEMP "cyclesrender_c4d_tests"
New-Item -ItemType Directory -Force $out | Out-Null
$plugins = "build\plugin\bin\Release\plugins"
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
  @{ Name = "test_geometry"; Covers = @("${B}scene/meshes.py", "${B}scene/instancers.py", "${B}scene/pointclouds.py", "${B}scene/curves.py", "${B}scene/materials.py") },
  @{ Name = "test_viewport"; Covers = @("${B}viewport/*") },
  @{ Name = "test_native"; Covers = @("${B}scene/native.py") },
  @{ Name = "test_preview"; Covers = @("${B}final/preview.py") },
  @{ Name = "test_cameras"; Covers = @("${B}scene/cameras.py") },
  @{ Name = "test_output"; Covers = @("${B}final/__init__.py", "${B}scene/objects.py", "${B}render/settings.py") },
  @{ Name = "measure_lights"; Covers = @("${B}scene/lights.py") },
  @{ Name = "test_lights"; Covers = @("${B}scene/lights.py") },
  @{ Name = "test_devices"; Covers = @("${B}render/devices.py", "${B}render/settings.py") }
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
  @{ Name = "passes"; Covers = @("${P}videopost/*", "${B}final/__init__.py", "${B}render/settings.py", "${B}scene/objects.py", "tools/make_settings.py", "${R}description/vpcyclesrender.*") },
  @{ Name = "region"; Covers = @("${P}videopost/cycles_videopost.*", "${B}final/__init__.py") },
  @{ Name = "volumes"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/volume_export.*", "${B}scene/volumes.py", "${P}tags/object_tag.*") },
  @{ Name = "hair_particles"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}scene/curves_export.*", "${B}scene/curves.py", "${B}scene/pointclouds.py", "${P}scene/document_colors.*") },
  @{ Name = "visibility"; Covers = @("${P}scene/light_export.*", "${P}scene/scene_exporter.*", "${B}scene/lights.py", "${B}scene/objects.py") },
  @{ Name = "redshift"; Covers = @("${P}scene/light_export.*", "${P}scene/camera_export.*", "${B}scene/lights.py", "${B}scene/cameras.py") },
  @{ Name = "object_tag"; Settings = @{ g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}tags/object_tag.*", "${R}description/tcyclesobject.*", "${R}strings_en-US/description/tcyclesobject.str", "${P}scene/light_export.*", "${P}scene/curves_export.*", "${B}scene/instancers.py", "${B}scene/objects.py") },
  @{ Name = "fire_vdb"; Covers = @("${P}scene/volume_export.*", "${B}scene/volumes.py", "${P}tags/object_tag.*") },
  @{ Name = "redshift_material"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}materials/redshift_material.*", "${P}materials/redshift_nodes.*", "${P}materials/node_graph.*", "${P}ipr/ipr_controller.*") },
  @{ Name = "redshift_nodes"; Covers = @("${P}materials/redshift_nodes.*", "${P}materials/redshift_material.*", "${P}materials/node_graph.*", "${B}scene/materials.py") },
  @{ Name = "redshift_dome"; Covers = @("${P}scene/environment_export.*", "${P}scene/light_export.*", "${B}scene/world.py") },
  @{ Name = "cycles_nodes"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}nodespace/*", "${P}materials/cycles_*", "${R}nodes/*", "tools/make_nodes.py", "tools/dump_blender_nodes.py", "${B}scene/materials.py", "${P}ipr/ipr_testhook.*") },
  @{ Name = "native_material"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderNativeMaterial = "true" }
     Covers = @("${P}materials/native_material.*", "${B}scene/native.py", "${R}description/mcyclesnative.*", "${R}strings_en-US/description/mcyclesnative.str", "${R}native_default.preview", "tools/make_native_placeholder.py") },
  @{ Name = "camera_tag"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}tags/camera_tag.*", "${R}description/tcyclescamera.*", "${R}strings_en-US/description/tcyclescamera.str", "${P}scene/camera_export.*",
                "${B}scene/cameras.py", "${P}ipr/ipr_controller.*", "${R}icons/*", "tools/make_icons.py") },
  @{ Name = "ipr"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_worker.*", "${P}ipr/ipr_testhook.*", "${P}ipr/ipr_trace.*", "${P}platform/process_stats*", "${B}viewport/*") },
  @{ Name = "ipr_reuse"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/scene_writer.*", "${P}scene/scene_exporter.*", "${P}scene/mesh_export.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_testhook.*",
                "${P}prefs/settings.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}session/protocol.*", "${B}protocol.py", "${B}scene/__init__.py") },
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
  @{ Name = "devices"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = "<file>" }
     Covers = @("${P}prefs/devices.*", "${P}prefs/prefs_page.*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str",
                "${P}videopost/render_settings_map.*", "${B}render/devices.py", "${B}render/settings.py", "${P}ipr/ipr_testhook.*") },
  @{ Name = "ipr_region"; Settings = @{ g_cyclesRenderIprTest = "true" }; Covers = @("${P}ipr/ipr_controller.*", "${P}ipr/ipr_worker.*", "${B}viewport/*") },
  @{ Name = "colour"; Settings = @{ g_cyclesRenderIprTest = "true" }
     Covers = @("${P}scene/document_colors.*", "${P}ipr/ipr_image_area.*", "${P}ipr/ipr_controller.*", "${P}materials/shader_translate.*", "${P}materials/material_export.*", "${B}render/settings.py") },
  @{ Name = "denoise"; Settings = @{ g_cyclesRenderIprTest = "true" }; Covers = @("${P}videopost/render_settings_*", "${P}ipr/ipr_controller.*", "${B}render/settings.py", "${B}viewport/*") },
  @{ Name = "baked_files"; Settings = @{ g_cyclesRenderIprTest = "true" }; GoneAfter = $true
     Covers = @("${P}materials/baked_files.*", "${P}platform/file_lock*", "${P}materials/shader_translate.*") },
  @{ Name = "ipr_window_checks"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/*", "${R}icons/*", "tools/make_icons.py", "${P}materials/material_preview.*", "${P}materials/baked_files.*", "${P}prefs/*", "${R}description/prefscyclesrender.*", "${R}strings_en-US/description/prefscyclesrender.str", "tests/c4d/harness/*") },
  @{ Name = "ipr_follow_window"; Window = $true; CleanPrefs = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}ipr/ipr_dialog.*", "${P}ipr/ipr_controller.*", "${P}ipr/ipr_trace.*", "${P}ipr/ipr_image_area.*", "${P}session/frame_watcher.*", "${B}viewport/*", "tests/c4d/harness/*") },
  @{ Name = "baked_files_window"; Window = $true; CleanPrefs = $true; GoneAfter = $true; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderBlender = $Blender }
     Covers = @("${P}materials/baked_files.*", "${P}materials/material_preview.*", "tests/c4d/harness/*") }
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
# does not reach is left to the sprint's full run.
$coreFiles = @(
  "${P}main.cpp", "${P}ids.h", "${P}version.h", "${P}scene/scene_exporter.*", "${P}scene/scene_writer.*", "${P}scene/parameters.h",
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

$allTests = @($bridgeTests | ForEach-Object { $_.Name }) + @($c4dTests | ForEach-Object { $_.Name })
foreach ($name in $Only) {
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

function Add-Result($name, $passed, $seconds, $note = "") {
  $script:results += [pscustomobject]@{ Test = $name; Result = $(if ($passed) { "passed" } else { "FAILED" }); Seconds = [int]$seconds }
  Write-Output ("{0,-22} {1} ({2:N0} s){3}" -f $name, $(if ($passed) { "passed" } else { "FAILED" }), $seconds, $note)
}

$buildSeconds = 0
if (-not $SkipBuild) {
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $PSScriptRoot "build_plugin.ps1") *> (Join-Path $out "build.log")
  # The exit code alone is not trusted: a build that fell back on old project files can end with 0.
  $built = ($LASTEXITCODE -eq 0) -and (Test-Path "$plugins\cyclesrender\cyclesrender.xdl64") -and
    -not (Select-String -Path (Join-Path $out "build.log") -Pattern "error C\d+|error :|LNK\d+|CMake Error" -Quiet)
  $buildSeconds = $watch.Elapsed.TotalSeconds
  Add-Result "build" $built $buildSeconds
  if (-not $built) { Write-Output "see $out\build.log"; exit 1 }
}

foreach ($test in $bridgeTests) {
  if (-not (Wanted $test.Name)) { continue }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  python "tests\bridge\$($test.Name).py" $Blender *> (Join-Path $out "$($test.Name).txt")
  Add-Result $test.Name ($LASTEXITCODE -eq 0) $watch.Elapsed.TotalSeconds
}

foreach ($start in $starts) {
  $first = $start.Tests[0]
  $settings = $start.Settings.Clone()
  foreach ($key in @($settings.Keys)) {
    if ($settings[$key] -ne "<file>") { continue }
    # A file of this start: gone before it begins (the runner also empties it before every test).
    $settings[$key] = Join-Path $out "$($start.Name)_$($key -replace '^g_cyclesRender', '').txt".ToLower()
    Remove-Item $settings[$key] -ErrorAction SilentlyContinue
  }
  if ($first.CleanPrefs) {
    $prefs = Join-Path $out "prefs_$($start.Name)"
    if (Test-Path $prefs) { Remove-Item $prefs -Recurse -Force }
    New-Item -ItemType Directory -Force $prefs | Out-Null
    $settings.g_prefsPath = $prefs
  }
  if ($start.Tests.Count -gt 1) {
    $shown = @($start.Settings.Keys | Sort-Object | ForEach-Object { "$($_)=$($start.Settings[$_])" }) -join " "
    Write-Output ("-- one start of c4dpy ({0}), in this order: {1}" -f $(if ($shown) { $shown } else { "no settings" }), (($start.Tests | ForEach-Object { $_.Name }) -join ", "))
  }
  $summary = Join-Path $out "$($start.Name).summary.json"
  Remove-Item $summary -ErrorAction SilentlyContinue
  $arguments = @{ Headless = (-not $first.Window); Script = @($start.Tests | ForEach-Object { "tests\c4d\$($_.Name).py" }); ModulePath = $plugins
                  TimeoutSec = 600; Settings = $settings; Name = $start.Name; SummaryFile = $summary }
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
Write-Output ("{0} of {1} passed" -f ($results.Count - $failed.Count), $results.Count)
Write-Output ("whole run {0:N0} s (build {1:N0} s)" -f $runWatch.Elapsed.TotalSeconds, $buildSeconds)
if ($failed.Count -gt 0 -or $left -gt 0) { exit 1 }
