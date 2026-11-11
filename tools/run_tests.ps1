# SPDX-License-Identifier: Apache-2.0
<#
Runs the project's whole test baseline and prints one line per test.

The tests themselves (the folder tests\) are kept locally and are not part of the repository;
without them this script has nothing to run. See docs\development.md, "Tests".

  .\tools\run_tests.ps1              build, bridge tests, then every Cinema 4D test (about 25 minutes)
  .\tools\run_tests.ps1 -SkipBuild   the same without rebuilding the plugin
  .\tools\run_tests.ps1 -Only lights,materials

Bridge tests need only Blender. Cinema 4D tests run in c4dpy (no window) with the plugin from
build\plugin and the bridge from the repository; the last one, ipr_window_checks, needs the IPR
window and starts the regular Cinema 4D for about half a minute. Details of a failed test: run it alone with
tests\c4d\run_c4d_test.ps1 (the command is in the test's first comment), or read its JSON
result in %TEMP%\cyclesrender_c4d_tests\<name>.result.json. Preview pictures of the comparison
tests are written to %TEMP%\cyclesrender_c4d_tests\images.
#>
param(
  [switch]$SkipBuild,
  [string[]]$Only = @(),
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
# The user may have a Blender of their own open; only processes that appear during the run count.
$blenderBefore = @(Get-Process blender -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })

function Add-Result($name, $passed, $seconds) {
  $script:results += [pscustomobject]@{ Test = $name; Result = $(if ($passed) { "passed" } else { "FAILED" }); Seconds = [int]$seconds }
  Write-Output ("{0,-22} {1} ({2:N0} s)" -f $name, $(if ($passed) { "passed" } else { "FAILED" }), $seconds)
}

function Wanted($name) { return ($Only.Count -eq 0) -or ($Only -contains $name) }

if (-not $SkipBuild) {
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & (Join-Path $PSScriptRoot "build_plugin.ps1") *> (Join-Path $out "build.log")
  # The exit code alone is not trusted: a build that fell back on old project files can end with 0.
  $built = ($LASTEXITCODE -eq 0) -and (Test-Path "$plugins\cyclesrender\cyclesrender.xdl64") -and
    -not (Select-String -Path (Join-Path $out "build.log") -Pattern "error C\d+|error :|LNK\d+|CMake Error" -Quiet)
  Add-Result "build" $built $watch.Elapsed.TotalSeconds
  if (-not $built) { Write-Output "see $out\build.log"; exit 1 }
}

# Bridge alone, no Cinema 4D
foreach ($name in "test_protocol", "test_render", "test_scene", "test_geometry", "test_viewport", "test_native", "test_preview", "measure_lights") {
  if (-not (Wanted $name)) { continue }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  python "tests\bridge\$name.py" $Blender *> (Join-Path $out "$name.txt")
  Add-Result $name ($LASTEXITCODE -eq 0) $watch.Elapsed.TotalSeconds
}

# Plugin inside Cinema 4D (c4dpy). name = script, settings for the run, and how the verdict is read.
$c4dTests = @(
  @{ Name = "session_selftest"; Settings = @{ g_cyclesRenderSelfTest = "4" }; ExpectLog = "Cycles Render self-test: PASSED all" },
  @{ Name = "traversal"; Settings = @{ g_cyclesRenderDumpFile = (Join-Path $out "traversal_dump.txt") } },
  @{ Name = "coords" }, @{ Name = "meshes" }, @{ Name = "cameras" }, @{ Name = "lights" }, @{ Name = "materials" },
  @{ Name = "sky_orientation" },
  @{ Name = "settings"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "settings_capture.txt") } },
  @{ Name = "sequence" }, @{ Name = "render_cancel" }, @{ Name = "golden" },
  @{ Name = "shaders" }, @{ Name = "mapping" },
  @{ Name = "node_materials"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "node_materials_capture.txt") } },
  @{ Name = "geometry"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "geometry_capture.txt") } },
  @{ Name = "motion_blur" }, @{ Name = "passes" }, @{ Name = "region" },
  @{ Name = "volumes"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "volumes_capture.txt") } },
  @{ Name = "hair_particles"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "hair_particles_capture.txt") } },
  @{ Name = "visibility" }, @{ Name = "redshift" },
  @{ Name = "object_tag"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "object_tag_capture.txt") } },
  @{ Name = "fire_vdb" },
  @{ Name = "redshift_material"; Settings = @{ g_cyclesRenderIprTest = "true" } },
  @{ Name = "redshift_dome" },
  @{ Name = "cycles_nodes"; Settings = @{ g_cyclesRenderCapture = (Join-Path $out "cycles_nodes_capture.txt") } },
  @{ Name = "native_material"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderNativeMaterial = "true" } },
  @{ Name = "ipr"; Settings = @{ g_cyclesRenderIprTest = "true"; g_cyclesRenderCapture = (Join-Path $out "ipr_capture.txt") } },
  @{ Name = "ipr_region"; Settings = @{ g_cyclesRenderIprTest = "true" } },
  @{ Name = "colour"; Settings = @{ g_cyclesRenderIprTest = "true" } },
  @{ Name = "denoise"; Settings = @{ g_cyclesRenderIprTest = "true" } },
  # Needs the real window: runs in the regular Cinema 4D, which opens and closes on screen.
  @{ Name = "ipr_window_checks"; Window = $true; Settings = @{ g_cyclesRenderIprTest = "true" } }
)
foreach ($test in $c4dTests) {
  if (-not (Wanted $test.Name)) { continue }
  $settings = if ($test.Settings) { $test.Settings } else { @{} }
  foreach ($file in $settings.Values) { if ($file -like "$out*") { Remove-Item $file -ErrorAction SilentlyContinue } }
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $arguments = @{ Headless = (-not $test.Window); Script = "tests\c4d\$($test.Name).py"; ModulePath = $plugins; TimeoutSec = 600; Settings = $settings }
  if ($test.ExpectLog) { $arguments.ExpectLog = $test.ExpectLog; $arguments.LogPattern = "Cycles Render self-test" }
  & "tests\c4d\run_c4d_test.ps1" @arguments *> (Join-Path $out "$($test.Name).txt")
  Add-Result $test.Name ($LASTEXITCODE -eq 0) $watch.Elapsed.TotalSeconds
}

$left = @(Get-Process blender -ErrorAction SilentlyContinue | Where-Object { $blenderBefore -notcontains $_.Id }).Count
Write-Output "blender processes left running by the tests: $left"
$failed = @($results | Where-Object { $_.Result -ne "passed" })
Write-Output ("{0} of {1} passed" -f ($results.Count - $failed.Count), $results.Count)
if ($failed.Count -gt 0 -or $left -gt 0) { exit 1 }
