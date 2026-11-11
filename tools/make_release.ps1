# SPDX-License-Identifier: Apache-2.0
<#
Builds the two packages of a release into release\<version>\.

  .\tools\make_release.ps1 -Version 0.1.0
  .\tools\make_release.ps1 -Version 0.1.0 -SkipBuild     # package the plugin build that is there

A release is the Cinema 4D plugin and the Blender bridge, built from the same sources and
carrying the same version number:

  release\<version>\CyclesRender-<version>-C4D2027-win64.zip   unpack into Cinema 4D's plugins folder
  release\<version>\cycles_render_bridge-<version>.zip         install in Blender (Install from Disk)
  release\<version>\cinema4d\cyclesrender\                     the plugin, unpacked
  release\<version>\blender\cycles_render_bridge\              the bridge, unpacked
  release\<version>\INSTALL.md                                 the installation steps

The version must already stand in the three places that carry it; the script checks them
against each other and changes nothing in the sources. What a previous run left in
release\<version>\ is replaced. release\README.md has the checklist around this script.
#>
param(
  [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
  [switch]$SkipBuild,
  # Used to validate the bridge package when it exists; the release is built without it too.
  [string]$Blender = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$out = Join-Path $repo "release\$Version"

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

# --- 2. The plugin binary. ---
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot "build_plugin.ps1") -Config Release
  if ($LASTEXITCODE -ne 0) { Write-Output "The plugin did not build; no release was made."; exit 1 }
}
$binary = Join-Path $repo "build\plugin\bin\Release\plugins\cyclesrender\cyclesrender.xdl64"
if (-not (Test-Path $binary)) { Write-Output "There is no built plugin at $binary. Run without -SkipBuild."; exit 1 }

# --- 3. Lay out both parts as they are installed. ---
# From here on a failed copy must stop the script: a package with a file missing is worse than none.
# (Not earlier: the build prints to the error stream, which this setting would take for a failure.)
$ErrorActionPreference = "Stop"
$pluginStage = Join-Path $out "cinema4d\cyclesrender"
$bridgeStage = Join-Path $out "blender\cycles_render_bridge"
$pluginZip = Join-Path $out "CyclesRender-$Version-C4D2027-win64.zip"
$bridgeZip = Join-Path $out "cycles_render_bridge-$Version.zip"
foreach ($old in (Join-Path $out "cinema4d"), (Join-Path $out "blender"), $pluginZip, $bridgeZip, (Join-Path $out "INSTALL.md")) {
  if (Test-Path $old) { Remove-Item $old -Recurse -Force }
}
New-Item -ItemType Directory -Force $pluginStage, $bridgeStage | Out-Null

# The plugin: the binary and the resources Cinema 4D loads at run time.
Copy-Item $binary $pluginStage
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

For Cinema 4D 2027 and Blender 5.2 on Windows. Install both parts.

## 1. The bridge, in Blender

1. Start Blender 5.2 and open *Edit > Preferences > Get Extensions*.
2. Open the menu at the top right of that page and choose *Install from Disk...*.
3. Pick ``cycles_render_bridge-$Version.zip``.
4. Close Blender. It does not need to be running; the Cinema 4D plugin starts it when it renders.

## 2. The plugin, in Cinema 4D

1. Unpack ``CyclesRender-$Version-C4D2027-win64.zip`` into a plugins folder of Cinema 4D 2027, for example the ``plugins`` folder inside the folder that *Edit > Preferences > Open Preferences Folder...* opens. It must then contain ``plugins/cyclesrender/cyclesrender.xdl64``.
2. Restart Cinema 4D.
3. In the Render Settings choose the renderer **Cycles Render**. The live render window is under *Extensions > Cycles Render IPR*.

On its first start the plugin looks for Blender 5.2 in its usual place. If yours is somewhere else, choose ``blender.exe`` in *Edit > Preferences > Renderer > Cycles Render*.

## Updating

Take both parts from the same release.

1. In Blender, open *Edit > Preferences > Add-ons*, unfold **Cycles Render Bridge** and press **Uninstall**. Then install the new .zip as above.
2. Replace the folder ``cyclesrender`` in Cinema 4D's plugins folder.
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
