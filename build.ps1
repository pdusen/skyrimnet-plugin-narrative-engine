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
#
# `build` / `rebuild` also prune orphaned Papyrus output. CMake's
# compile_papyrus target invokes PapyrusCompiler.exe with `-all`, which only
# ever *writes* .pex files into <mod-folder>/Scripts/ — it never removes the
# output of a .psc that has since been deleted. Since package.ps1 zips the
# deployed mod folder wholesale, an orphaned .pex would ship in the release
# and, worse, could override a same-named vanilla or third-party script. So
# after a successful build we delete any .pex in the deployed Scripts folder
# with no matching .psc under esp/Source/Scripts/ (basename match — the
# compiler names output after the Scriptname declaration, which matches the
# filename for every script in this project).

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
    Invoke-PapyrusPrune
}

# Delete .pex files in the deployed mod folder that no longer have a
# corresponding .psc source. See the header comment for why this is needed.
function Invoke-PapyrusPrune {
    $modsRoot = $env:SKYRIM_MODS_FOLDER
    if (-not $modsRoot) { return }

    $deployedScripts = Join-Path $modsRoot 'NarrativeEngine/Scripts'
    if (-not (Test-Path -LiteralPath $deployedScripts -PathType Container)) { return }

    $sourceScripts = Join-Path $PSScriptRoot 'esp/Source/Scripts'
    $sourceNames = @(
        Get-ChildItem -LiteralPath $sourceScripts -Filter '*.psc' -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.BaseName }
    )

    # Refuse to prune against an empty source list. If esp/Source/Scripts/ is
    # missing or misresolved, every deployed .pex would look orphaned and we'd
    # wipe the whole Scripts folder. A project with genuinely zero .psc files
    # has nothing to deploy anyway, so skipping costs nothing.
    if ($sourceNames.Count -eq 0) {
        Write-Warning "Papyrus prune: no .psc sources found under $sourceScripts; skipping prune."
        return
    }

    $orphans = @(
        Get-ChildItem -LiteralPath $deployedScripts -Filter '*.pex' -File -ErrorAction SilentlyContinue |
            Where-Object { $sourceNames -notcontains $_.BaseName }
    )
    if ($orphans.Count -eq 0) { return }

    Write-Host "==> pruning $($orphans.Count) orphaned .pex from $deployedScripts" -ForegroundColor Cyan
    foreach ($orphan in $orphans) {
        Write-Host "    removed $($orphan.Name)" -ForegroundColor DarkGray
        Remove-Item -LiteralPath $orphan.FullName -Force
    }
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
