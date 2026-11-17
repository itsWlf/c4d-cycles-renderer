# SPDX-License-Identifier: Apache-2.0
<#
Configures (on first use) and builds the Cycles Render module against the Cinema 4D C++ SDK of one
Cinema 4D version, or of all supported versions.

  .\tools\build_plugin.ps1                  # Release build for Cinema 4D 2027
  .\tools\build_plugin.ps1 -Version 2025    # for Cinema 4D 2025
  .\tools\build_plugin.ps1 -Version all     # for 2024, 2025, 2026 and 2027, one after the other
  .\tools\build_plugin.ps1 -Config Debug
  .\tools\build_plugin.ps1 -Reconfigure     # after changing CMake settings or the SDK

Where each build goes (the module of one version is never loaded by another, docs\reference\c4d-sdk.md,
"Cinema 4D versions"):

  2027  build\plugin\bin\<Config>\plugins\cyclesrender\cyclesrender.xdl64
  2026  build\plugin-2026\bin\<Config>\plugins\cyclesrender\cyclesrender.xdl64   (likewise 2025, 2024)

The SDKs are not part of this repository. Unpack sdk.zip from each Cinema 4D installation folder
and name the result with -Sdk (one version only), with the environment variable C4D_SDK (2027) or
C4D_SDK_<year> (any version), or put it where the script looks by default:

  2027  references\Cinema_4D_CPP_SDK_2027_0_0
  2026  references\Cinema 4D 2026_sdk
  2025  references\Cinema 4D 2025_sdk
  2024  references\Cinema 4D 2024_sdk   (and the 2025 SDK as well, see below)

How the versions differ in their build set-up (measured, docs\reference\c4d-sdk.md):

  2027  The module folder plugin\cyclesrender is built as it is (project\CMakeLists.txt).
  2025, 2026  These SDKs read a module from project\projectdefinition.txt and would take a
        CMakeLists.txt next to it as their own. The script therefore lays out a module folder of
        its own in the build folder: a projectdefinition.txt written from the settings in
        project\CMakeLists.txt (one list of frameworks for all versions), and junctions to the
        repository's source and res folders.
  2024  The 2024 SDK has no CMake set-up (it came with Maxon's Project Tool, not on this machine).
        The script copies it to build\sdk\2024, adds the CMake files of the 2025 SDK, which read
        the same projectdefinition.txt files, and changes two things in the copy: the source
        processor gets the 2025 SDK's fix for a construct newer compilers reject ("using
        decltype(...)::Name", error C7810), the instruction set is AVX as in the 2024 SDK's own
        settings (the 2025 files say AVX2), and warning C5264, which a macro of the 2024
        framework raises in Maxon's own code, is muted. It compiles as C++17, as the 2024 SDK's settings
        say, and with cinema defined as nothing: 2024 has the classic API in the global
        namespace, so the sources' cinema::X becomes ::X (source\sdk_compat.h).

The SDK folders stay untouched: all output goes to build\, and Python is told not to write
bytecode caches next to the SDKs' tools.

The defaults for generator, toolset and Windows SDK are those the project is developed with
(Visual Studio 2026 with the v143 toolset, for every version). With Visual Studio 2022 pass
  -Generator "Visual Studio 17 2022" -Architecture x64
Changing them needs -Reconfigure, or a fresh build folder.
#>
param(
  [ValidateSet("2024", "2025", "2026", "2027", "all")][string]$Version = "2027",
  [ValidateSet("Release", "Debug")][string]$Config = "Release",
  [switch]$Reconfigure,
  [string]$Sdk = "",
  [string]$Generator = "Visual Studio 18 2026",
  [string]$Toolset = "v143",
  [string]$Architecture = "x64,version=10.0.26100.0"
)

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:PYTHONDONTWRITEBYTECODE = "1"

if ($Version -eq "all") {
  if ($Sdk) { Write-Output "-Sdk names the SDK of one version; with -Version all use C4D_SDK_<year> or the default folders."; exit 1 }
  $failed = @()
  foreach ($one in "2027", "2026", "2025", "2024") {
    Write-Output "=== Cinema 4D $one"
    & $PSCommandPath -Version $one -Config $Config -Reconfigure:$Reconfigure -Generator $Generator -Toolset $Toolset -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { $failed += $one }
  }
  if ($failed.Count) { Write-Output ("=== failed: Cinema 4D {0}" -f ($failed -join ", ")); exit 1 }
  Write-Output "=== built for Cinema 4D 2024, 2025, 2026 and 2027"
  exit 0
}

$defaultSdk = @{
  "2027" = "references\Cinema_4D_CPP_SDK_2027_0_0"; "2026" = "references\Cinema 4D 2026_sdk"
  "2025" = "references\Cinema 4D 2025_sdk"; "2024" = "references\Cinema 4D 2024_sdk"
}
function Find-Sdk($year, $given) {
  if ($given) { return $given }
  $fromEnv = [Environment]::GetEnvironmentVariable("C4D_SDK_$year")
  if (-not $fromEnv -and $year -eq "2027") { $fromEnv = $env:C4D_SDK }
  if ($fromEnv) { return $fromEnv }
  return Join-Path $repo $defaultSdk[$year]
}
$Sdk = Find-Sdk $Version $Sdk
# The 2024 SDK has frameworks but no CMakeLists.txt; every other one has both.
$marker = if ($Version -eq "2024") { "frameworks\core.framework" } else { "CMakeLists.txt" }
if (-not (Test-Path (Join-Path $Sdk $marker))) {
  Write-Output "The Cinema 4D $Version C++ SDK was not found at '$Sdk'."
  Write-Output "Unpack sdk.zip from the Cinema 4D $Version installation folder and pass its folder with -Sdk (or set C4D_SDK_$Version)."
  exit 1
}

# --- The 2024 SDK: a copy with the 2025 SDK's CMake files -------------------------------------------
# What is copied and changed is described at the top. The copy is made again when the SDK it was
# made from, or this recipe, changes (the stamp file names both).
function Initialize-Sdk2024($source) {
  $sdk2025 = Find-Sdk "2025" ""
  if (-not (Test-Path (Join-Path $sdk2025 "cmake\sdk_update_projects.cmake"))) {
    Write-Host "Building for Cinema 4D 2024 needs the CMake files of the 2025 SDK as well; it was not found at '$sdk2025' (set C4D_SDK_2025)."
    return $null
  }
  $target = Join-Path $repo "build\sdk\2024"
  $stamp = Join-Path $target "cyclesrender_prepared.txt"
  $recipe = "recipe 2: 2025 CMake files, generator fix C7810, /arch:AVX, /wd5264`n2024 SDK: $source`n2025 SDK: $sdk2025"
  if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $recipe.Trim())) { return $target }
  Write-Host "Preparing the 2024 SDK in $target (a copy with the 2025 SDK's CMake files)"
  if (Test-Path $target) { Remove-Item $target -Recurse -Force }
  New-Item -ItemType Directory -Force $target | Out-Null
  robocopy $source $target /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Write-Host "Copying the 2024 SDK failed (robocopy $LASTEXITCODE)."; return $null }
  foreach ($file in "CMakeLists.txt", "CMakePresets.json", "custom_paths.txt") { Copy-Item (Join-Path $sdk2025 $file) $target }
  Copy-Item (Join-Path $sdk2025 "cmake") $target -Recurse
  # The 2025 source processor's way of writing these using-declarations, which compilers accept.
  $generator = Join-Path $target "frameworks\settings\sourceprocessor\generators\cppgenerator.py"
  $text = [IO.File]::ReadAllText($generator)
  $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
  $old = "                        for uc, un in usings:$nl                            self.decls << 'using decltype(PrivateBaseClass::PrivateLookupFn((' + uc + '**) nullptr))::' + un + ';'"
  $new = (@(
    "                        declaredBases = set()",
    "                        for uc, un in usings:",
    "                            uc2 = ''.join([c if c.isalnum() else '_' for c in uc])",
    "                            if uc not in declaredBases:",
    "                                declaredBases.add(uc)",
    "                                self.decls << 'using RefFnClass_' + uc2 + ' = decltype(PrivateBaseClass::PrivateLookupFn((' + uc + '**) nullptr));'",
    "                            self.decls << 'using RefFnClass_' + uc2 + '::' + un + ';'") -join $nl)
  if (-not $text.Contains($old)) { Write-Host "The 2024 source processor does not look as expected ($generator); not prepared."; return $null }
  [IO.File]::WriteAllText($generator, $text.Replace($old, $new))
  $options = Join-Path $target "cmake\base_settings\compile_options.cmake"
  $text = [IO.File]::ReadAllText($options)
  $old = "set(MAXON_COMPILE_OPTIONS_WINDOWS_MSVC_X64_ISA /arch:AVX2)"
  if (-not $text.Contains($old)) { Write-Host "The 2025 compile options do not look as expected ($options); not prepared."; return $null }
  [IO.File]::WriteAllText($options, $text.Replace($old, "set(MAXON_COMPILE_OPTIONS_WINDOWS_MSVC_X64_ISA /arch:AVX) # Cinema 4D 2024's own settings (Base.props): AVX"))
  # The 2024 framework's MAXON_DEPENDENCY_ON_MODULE leaves a const variable unused (C5264 in
  # cinema.framework's c4d_pmain.cpp, Maxon's code); the 2025 SDK's does not.
  $warnings = Join-Path $target "cmake\base_settings\warnings.msvc.cmake"
  $text = [IO.File]::ReadAllText($warnings)
  $old = "set(MUTED_WARNINGS_MSVC"
  if (-not $text.Contains($old)) { Write-Host "The 2025 warning settings do not look as expected ($warnings); not prepared."; return $null }
  [IO.File]::WriteAllText($warnings, $text.Replace($old, "$old$nl/wd5264 # 2024 SDK: MAXON_DEPENDENCY_ON_MODULE leaves an unused const variable"))
  Set-Content -Path $stamp -Value $recipe -Encoding utf8
  return $target
}

# --- A module folder for the SDKs that read projectdefinition.txt (2024 to 2026) --------------------
# projectdefinition.txt is written from target_settings in project\CMakeLists.txt, so that the
# frameworks, the module id and the style check level stand in one place.
function Initialize-LegacyModule($buildFolder, $year) {
  $cmakeFile = Join-Path $repo "plugin\cyclesrender\project\CMakeLists.txt"
  $text = [IO.File]::ReadAllText($cmakeFile)
  if ($text -notmatch '(?s)set\(target_settings \[=\[(.*?)\]=\]\)') { Write-Host "No target_settings in $cmakeFile."; return $null }
  $settings = $Matches[1] | ConvertFrom-Json
  $lines = @(
    "// Written by tools\build_plugin.ps1 from plugin\cyclesrender\project\CMakeLists.txt for the Cinema 4D $year SDK. Do not edit.",
    "Platform=Win64",
    "Type=DLL",
    ("APIS=" + ($settings.apis -join ";")),
    "C4D=true",
    "stylecheck.level=$($settings.'stylecheck.level')",
    "ModuleId=$($settings.moduleid)"
  )
  # 2024: the classic API has no namespace (see the top of this script and source\sdk_compat.h).
  if ($year -eq "2024") { $lines += "AdditionalPreprocessorDefinitions=cinema=" }
  $modules = Join-Path $buildFolder "modules"
  $module = Join-Path $modules "cyclesrender"
  New-Item -ItemType Directory -Force (Join-Path $module "project") | Out-Null
  $definition = Join-Path $module "project\projectdefinition.txt"
  $content = ($lines -join "`r`n") + "`r`n"
  if (-not (Test-Path $definition) -or [IO.File]::ReadAllText($definition) -ne $content) { [IO.File]::WriteAllText($definition, $content) }
  foreach ($name in "source", "res") {
    $link = Join-Path $module $name
    if (-not (Test-Path $link)) { New-Item -ItemType Junction -Path $link -Target (Join-Path $repo "plugin\cyclesrender\$name") | Out-Null }
  }
  return $modules
}

$extra = @()
if ($Version -eq "2027") {
  $build = Join-Path $repo "build\plugin"
  $modulesDir = Join-Path $repo "plugin"
}
else {
  $build = Join-Path $repo "build\plugin-$Version"
  if ($Version -eq "2024") {
    $Sdk = Initialize-Sdk2024 $Sdk
    if (-not $Sdk) { exit 1 }
    $extra += "-DCMAKE_CXX_STANDARD=17"
  }
  New-Item -ItemType Directory -Force $build | Out-Null
  $modulesDir = Initialize-LegacyModule $build $Version
  if (-not $modulesDir) { exit 1 }
}

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

# The SDKs of 2024 to 2026 list a module's source files when the project is configured, and a build
# does not notice a file added or removed since (found 2026-11-16: the build of a new file failed to
# link in all three until -Reconfigure). The list is kept next to the cache, and a different one
# configures the project again. The 2027 SDK notices by itself (the second build below).
$sourceStamp = $null
if ($Version -ne "2027") {
  $sourceList = (@(Get-ChildItem (Join-Path $repo "plugin\cyclesrender\source") -Recurse -File -Include *.cpp, *.h |
    ForEach-Object { $_.FullName.Substring($repo.Length) }) | Sort-Object) -join "`n"
  $sourceStamp = Join-Path $build "source_files.txt"
  if (-not (Test-Path $sourceStamp) -or [IO.File]::ReadAllText($sourceStamp) -ne $sourceList) { $Reconfigure = $true }
}

if ($Reconfigure -or -not (Test-Path $cache)) {
  cmake @fresh -S $Sdk -B $build -G $Generator -T $Toolset -A $Architecture "-DMAXON_SDK_MODULES_DIR=$modulesDir" @extra
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  if ($sourceStamp) { [IO.File]::WriteAllText($sourceStamp, $sourceList) }
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
