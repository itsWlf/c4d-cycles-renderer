# SPDX-License-Identifier: Apache-2.0
<#
Configures (on first use) and builds the Cycles Render module against the Cinema 4D C++ SDK.

  .\tools\build_plugin.ps1                  # Release build
  .\tools\build_plugin.ps1 -Config Debug
  .\tools\build_plugin.ps1 -Reconfigure     # after changing CMake settings or the SDK

The built module is build\plugin\bin\<Config>\plugins\cyclesrender\cyclesrender.xdl64.

The Cinema 4D SDK is not part of this repository. Unpack sdk.zip from the Cinema 4D 2027
installation folder and name the result with -Sdk, with the environment variable C4D_SDK, or put
it where the script looks by default: references\Cinema_4D_CPP_SDK_2027_0_0.

The SDK is used as the CMake source tree and stays untouched: all output goes to build\plugin,
and Python is told not to write bytecode caches next to the SDK's tools.

The defaults for generator, toolset and Windows SDK are those the project is developed with
(Visual Studio 2026 with the v143 toolset). With Visual Studio 2022 pass
  -Generator "Visual Studio 17 2022" -Architecture x64
(the SDK's own presets expect that version). Changing them needs -Reconfigure, or a fresh
build folder.
#>
param(
  [ValidateSet("Release", "Debug")][string]$Config = "Release",
  [switch]$Reconfigure,
  [string]$Sdk = "",
  [string]$Generator = "Visual Studio 18 2026",
  [string]$Toolset = "v143",
  [string]$Architecture = "x64,version=10.0.26100.0"
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Sdk) { $Sdk = $env:C4D_SDK }
if (-not $Sdk) { $Sdk = Join-Path $repo "references\Cinema_4D_CPP_SDK_2027_0_0" }
if (-not (Test-Path (Join-Path $Sdk "CMakeLists.txt"))) {
  Write-Output "The Cinema 4D C++ SDK was not found at '$Sdk'."
  Write-Output "Unpack sdk.zip from the Cinema 4D 2027 installation folder and pass its folder with -Sdk (or set C4D_SDK)."
  exit 1
}
$build = Join-Path $repo "build\plugin"
$env:PYTHONDONTWRITEBYTECODE = "1"

# A build folder remembers the absolute paths it was configured with. After the project was
# moved or copied, building with the old cache would compile the sources of the old location
# and write its output there. Such a cache is thrown away and the folder configured afresh.
$cache = Join-Path $build "CMakeCache.txt"
$fresh = @()
if (Test-Path $cache) {
  $line = Select-String -Path $cache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.*)$' | Select-Object -First 1
  $cachedFor = if ($line) { $line.Matches[0].Groups[1].Value } else { "" }
  if ($cachedFor -and ([IO.Path]::GetFullPath($cachedFor).TrimEnd('\') -ne [IO.Path]::GetFullPath($build).TrimEnd('\'))) {
    Write-Output "The build folder was configured for another location ($cachedFor). Configuring it again for this one."
    $Reconfigure = $true
    $fresh = @("--fresh")
  }
}

if ($Reconfigure -or -not (Test-Path $cache)) {
  cmake @fresh -S $Sdk -B $build -G $Generator -T $Toolset -A $Architecture "-DMAXON_SDK_MODULES_DIR=$repo\plugin"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# /nr:false: no lingering MSBuild worker processes that would keep an old environment.
function Invoke-Build {
  cmake --build $build --config $Config --target cyclesrender -- /m /nr:false /v:minimal 2>&1 | Tee-Object -Variable buildOutput
  $script:regenerated = [bool]($buildOutput | Select-String "Generating done")
  return
}

Invoke-Build
if ($LASTEXITCODE -ne 0 -and $script:regenerated) {
  # New or removed source files make CMake regenerate the project during the build; that
  # build still uses the old file list and fails to link. The second build is the real one.
  Write-Output "--- project was regenerated; building again"
  Invoke-Build
}
exit $LASTEXITCODE
