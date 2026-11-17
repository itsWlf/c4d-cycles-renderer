# SPDX-License-Identifier: Apache-2.0
<#
Builds the two packages of a release into release\<version>\.

  .\tools\make_release.ps1 -Version 0.1.0
  .\tools\make_release.ps1 -Version 0.1.0 -SkipBuild     # package the plugin builds that are there
  .\tools\make_release.ps1 -Version 0.1.0 -OutDir build\release_try   # elsewhere than release\0.1.0

A release is the Cinema 4D plugin and the Blender bridge, built from the same sources and
carrying the same version number:

  release\<version>\CyclesRender-<version>-C4D2024-2027-win64.zip   unpack into Cinema 4D's plugins folder
  release\<version>\cycles_render_bridge-<version>.zip              install in Blender (Install from Disk)
  release\<version>\cinema4d\cyclesrender\                          the plugin, unpacked
  release\<version>\blender\cycles_render_bridge\                   the bridge, unpacked
  release\<version>\INSTALL.md                                      the installation steps

The plugin folder holds one build per supported Cinema 4D version, cyclesrender_2024.xdl64 to
cyclesrender_2027.xdl64, and one res folder. Every Cinema 4D loads the build of its own version
and passes over the others without a message (measured in 2024 to 2027; docs\reference\c4d-sdk.md,
"Cinema 4D versions"), so the one folder is installed unchanged into any of them.

The version must already stand in the three places that carry it; the script checks them
against each other and changes nothing in the sources. What a previous run left in
release\<version>\ is replaced. release\README.md has the checklist around this script.
#>
param(
  [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
  [switch]$SkipBuild,
  # Used to validate the bridge package when it exists; the release is built without it too.
  [string]$Blender = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe",
  # Where to write instead of release\<version>, for trying the packaging without replacing the
  # packages of a release made earlier with the same number.
  [string]$OutDir = ""
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$out = if (-not $OutDir) { Join-Path $repo "release\$Version" } elseif ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repo $OutDir }

# --- 1. The version number stands in three places; all must agree with the one asked for. ---
function Read-Version($file, $pattern) {
  $match = Select-String -Path (Join-Path $repo $file) -Pattern $pattern | Select-Object -First 1
  if (-not $match) { throw "No version number found in $file." }
  return $match.Matches[0].Groups[1].Value
}
$versions = [ordered]@{
  "plugin\cyclesrender\source\version.h" = Read-Version "plugin\cyclesrender\source\version.h" '#define\s+CYCLESRENDER_VERSION\s+"([^"]+)"'
  "bridge\blender_manifest.toml"         = Read-Version "bridge\blender_manifest.toml" '^version\s*=\s*"([^"]+)"'
  "bridge\c4d_bridge\__init__.py"        = Read-Version "bridge\c4d_bridge\__init__.py" '^BRIDGE_VERSION\s*=\s*"([^"]+)"'
}
$wrong = @($versions.GetEnumerator() | Where-Object { $_.Value -ne $Version })
if ($wrong.Count -gt 0) {
  $wrong | ForEach-Object { Write-Output ("{0} says version {1}, not {2}." -f $_.Key, $_.Value, $Version) }
  Write-Output "Set the version in all three places first (release\README.md, checklist)."
  exit 1
}

# --- 2. The plugin binaries, one per Cinema 4D version. ---
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot "build_plugin.ps1") -Version all -Config Release
  if ($LASTEXITCODE -ne 0) { Write-Output "The plugin did not build for every Cinema 4D version; no release was made."; exit 1 }
}
$binaries = [ordered]@{}
foreach ($year in "2024", "2025", "2026", "2027") {
  $folder = if ($year -eq "2027") { "build\plugin" } else { "build\plugin-$year" }
  $binaries[$year] = Join-Path $repo "$folder\bin\Release\plugins\cyclesrender\cyclesrender.xdl64"
  if (-not (Test-Path $binaries[$year])) { Write-Output "There is no built plugin for Cinema 4D $year at $($binaries[$year]). Run without -SkipBuild."; exit 1 }
}

# --- 3. Lay out both parts as they are installed. ---
# From here on a failed copy must stop the script: a package with a file missing is worse than none.
# (Not earlier: the build prints to the error stream, which this setting would take for a failure.)
$ErrorActionPreference = "Stop"
$pluginStage = Join-Path $out "cinema4d\cyclesrender"
$bridgeStage = Join-Path $out "blender\cycles_render_bridge"
$pluginZip = Join-Path $out "CyclesRender-$Version-C4D2024-2027-win64.zip"
$bridgeZip = Join-Path $out "cycles_render_bridge-$Version.zip"
$oldZips = @(Get-ChildItem $out -Filter "CyclesRender-$Version-C4D*-win64.zip" -ErrorAction SilentlyContinue | ForEach-Object FullName)
foreach ($old in @((Join-Path $out "cinema4d"), (Join-Path $out "blender"), $pluginZip, $bridgeZip, (Join-Path $out "INSTALL.md")) + $oldZips) {
  if (Test-Path $old) { Remove-Item $old -Recurse -Force }
}
New-Item -ItemType Directory -Force $pluginStage, $bridgeStage | Out-Null

# The plugin: one binary per Cinema 4D version, named after it, and the resources all of them load.
foreach ($year in $binaries.Keys) { Copy-Item $binaries[$year] (Join-Path $pluginStage "cyclesrender_$year.xdl64") }
Copy-Item (Join-Path $repo "plugin\cyclesrender\res") $pluginStage -Recurse
$pluginLicence = @("plugin\LICENSE", "LICENSE") | ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($pluginLicence) { Copy-Item $pluginLicence (Join-Path $pluginStage "LICENSE.txt") }
else { throw "The licence file of the plugin (LICENSE in the repository root) is missing." }

# The bridge: the extension as Blender installs it. Bytecode caches stay out.
foreach ($file in "blender_manifest.toml", "__init__.py", "LICENSE", "README.md") {
  Copy-Item (Join-Path $repo "bridge\$file") $bridgeStage
}
$bridgeSource = Join-Path $repo "bridge\c4d_bridge"
Get-ChildItem $bridgeSource -Recurse -File -Filter *.py | Where-Object { $_.FullName -notmatch '\\__pycache__\\' } | ForEach-Object {
  $target = Join-Path $bridgeStage ("c4d_bridge" + $_.FullName.Substring($bridgeSource.Length))
  New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
  Copy-Item $_.FullName $target
}

# --- 4. The archives. Entry names are written with forward slashes, as the zip format asks. ---
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function New-Zip($folder, $zipFile, $prefix) {
  $archive = [System.IO.Compression.ZipFile]::Open($zipFile, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    Get-ChildItem $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
      $name = $prefix + $_.FullName.Substring($folder.Length + 1).Replace('\', '/')
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $name, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  }
  finally { $archive.Dispose() }
}
New-Zip $pluginStage $pluginZip "cyclesrender/"   # unpacks as the folder "cyclesrender"
New-Zip $bridgeStage $bridgeZip ""                 # Blender wants the manifest at the top

# --- 5. The installation steps, for the release notes. ---
@"
# Cycles Render $Version

For Cinema 4D 2024, 2025, 2026 or 2027 and Blender 5.0, 5.1, 5.2 or 5.3 on Windows. Install both parts.

## 1. The bridge, in Blender

1. Start Blender (5.0 to 5.3) and open *Edit > Preferences > Get Extensions*.
2. Open the menu at the top right of that page and choose *Install from Disk...*.
3. Pick ``cycles_render_bridge-$Version.zip``.
4. Close Blender. It does not need to be running; the Cinema 4D plugin starts it when it renders.

## 2. The plugin, in Cinema 4D

1. Unpack ``CyclesRender-$Version-C4D2024-2027-win64.zip`` into a plugins folder of Cinema 4D, for example the ``plugins`` folder inside the folder that *Edit > Preferences > Open Preferences Folder...* opens. It must then contain ``plugins/cyclesrender/res`` and the four files ``cyclesrender_2024.xdl64`` to ``cyclesrender_2027.xdl64``.
2. Restart Cinema 4D. The console (*Extensions > Console*) says ``Cycles Render $Version for Cinema 4D <year>: module loaded``.
3. In the Render Settings choose the renderer **Cycles Render**. The live render window is under *Extensions > Cycles Render IPR*.

The folder holds one build for each Cinema 4D version, and every Cinema 4D loads the one for its own version and passes over the others: keep all four, and install the same folder into every Cinema 4D you use. The command-line renderer (``Commandline.exe``) names the builds it passes over in its output ("WARNING: Error loading ...cyclesrender_2024.xdl64 ... has wrong binary version", "Skipping module ..."); those lines are expected and the render goes on with the right build. Each version is built with the last update of its year (2024.5, 2025.3, 2026.3, 2027.0); an earlier update of a year may not load it, so keep Cinema 4D up to date.

On its first start the plugin looks for a supported Blender in its usual place (of several, the one the bridge is installed into, else Blender 5.2 (the long-term support release), else the newest). If yours is somewhere else, choose ``blender.exe`` in *Edit > Preferences > Renderer > Cycles Render*.

## Updating

Take both parts from the same release.

1. In Blender, open *Edit > Preferences > Add-ons*, unfold **Cycles Render Bridge** and press **Uninstall**. Then install the new .zip as above.
2. Delete the folder ``cyclesrender`` in Cinema 4D's plugins folder, then unpack the new one. Do not unpack over the old folder: a ``cyclesrender.xdl64`` of 0.1.0 left in it would compete with the new build of the same Cinema 4D version.
"@ | Set-Content -Path (Join-Path $out "INSTALL.md") -Encoding utf8

# --- 6. Let Blender check the bridge package, when Blender is there. ---
$ErrorActionPreference = "Continue"
if (Test-Path $Blender) {
  & $Blender --command extension validate $bridgeZip
  if ($LASTEXITCODE -ne 0) { Write-Output "Blender does not accept the bridge package."; exit 1 }
}
else { Write-Output "note: Blender was not found at '$Blender'; the bridge package was not validated." }

Write-Output ""
Write-Output "Release $Version is in $out"
Get-ChildItem $out -File | ForEach-Object { Write-Output ("  {0,-48} {1,10:N0} bytes" -f $_.Name, $_.Length) }
