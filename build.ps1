# build.ps1 — convenience wrapper for CLI-driven builds.
#
# Loads the Visual Studio Developer environment once, then forwards to cmake
# with the right preset. Mirrors what VS Code's CMake Tools extension does
# under the hood, but from a plain PowerShell session.
#
# Usage:
#   pwsh -File build.ps1 configure              # cmake --preset local-release
#   pwsh -File build.ps1 build                  # cmake --build build/local-release
#   pwsh -File build.ps1 rebuild                # configure + build (no clean)
#   pwsh -File build.ps1 clean                  # remove build/<preset>
#   pwsh -File build.ps1 build -Preset local-debug
#
# Default preset is `local-release`. Debug builds don't currently work at
# runtime: SkyrimNet's exported APIs return `std::string` by value, and a
# debug-CRT (`/MTd`, `_ITERATOR_DEBUG_LEVEL=2`) build of NarrativeEngine has
# an incompatible string ABI with SkyrimNet's release build — the destructor
# crashes when our DLL tries to free a buffer allocated in theirs. So we
# default to release for everyday testing; `-Preset local-debug` is still
# available for the rare case where you want STL asserts on code paths that
# don't touch SkyrimNet.
#
# Each invocation pays the dev-shell load cost (~1-2 s) and the configure
# step's cmake/vcpkg cost on first run; subsequent builds reuse the cache.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('configure', 'build', 'rebuild', 'clean')]
    [string]$Verb = 'build',

    [string]$Preset = 'local-release'
)

$ErrorActionPreference = 'Stop'

# --- 1. Locate Launch-VsDevShell.ps1 -----------------------------------------

$launchScript = Get-ChildItem `
    -Path "C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1" `
    -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $launchScript) {
    throw "Couldn't find Launch-VsDevShell.ps1 under 'C:\Program Files\Microsoft Visual Studio\2022\*'. Update build.ps1 if Visual Studio is installed elsewhere."
}

# --- 2. Load the VS Developer environment (silent) ---------------------------

& $launchScript.FullName -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

# Launch-VsDevShell.ps1 changes the working directory; restore it.
Set-Location $PSScriptRoot

# It also clobbers VCPKG_ROOT if VS bundles a vcpkg. CMakeUserPresets.json
# carries the right value for the preset itself, but native cmake calls in
# this session still see the env var. Re-pin it from the preset file so the
# two stay in sync.
$presetFile = Join-Path $PSScriptRoot 'CMakeUserPresets.json'
if (Test-Path $presetFile) {
    try {
        $userPresets = Get-Content $presetFile -Raw | ConvertFrom-Json
        $matching = $userPresets.configurePresets | Where-Object { $_.name -eq $Preset }
        if ($matching -and $matching.environment) {
            foreach ($prop in $matching.environment.PSObject.Properties) {
                Set-Item -Path "Env:$($prop.Name)" -Value $prop.Value
            }
        }
    }
    catch {
        Write-Warning "Couldn't parse CMakeUserPresets.json for preset '$Preset': $_"
    }
}

$buildDir = Join-Path $PSScriptRoot "build/$Preset"

# --- 3. Dispatch -------------------------------------------------------------

function Invoke-Configure {
    Write-Host "==> cmake --preset $Preset" -ForegroundColor Cyan
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "Configure failed (exit $LASTEXITCODE)." }
}

function Invoke-Build {
    if (-not (Test-Path "$buildDir/CMakeCache.txt")) {
        Write-Host "==> build dir not configured; running configure first" -ForegroundColor Yellow
        Invoke-Configure
    }
    Write-Host "==> cmake --build $buildDir" -ForegroundColor Cyan
    cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
}

function Invoke-Clean {
    if (Test-Path $buildDir) {
        Write-Host "==> removing $buildDir" -ForegroundColor Cyan
        Remove-Item -Recurse -Force $buildDir
    }
    else {
        Write-Host "==> $buildDir does not exist; nothing to clean" -ForegroundColor DarkGray
    }
}

switch ($Verb) {
    'configure' { Invoke-Configure }
    'build' { Invoke-Build }
    'rebuild' { Invoke-Configure; Invoke-Build }
    'clean' { Invoke-Clean }
}

Write-Host "==> done ($Verb / $Preset)" -ForegroundColor Green
