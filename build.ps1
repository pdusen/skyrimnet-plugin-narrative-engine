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
#   pwsh -File build.ps1 test                   # build + run the Catch2 unit tests
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
# `build` / `rebuild` deploy through a staging folder, build/<preset>/staging/,
# laid out exactly as the mod folder is. Every CMake deploy rule (DLL, statics,
# dashboard bundle, compiled Papyrus) writes there and nowhere else. Each build:
#
#   1. empties the staging folder, so it can only ever hold what this build
#      produced;
#   2. runs the CMake build, which fills it;
#   3. adds NarrativeEngine.esp, copied from the mod folder -- the one file
#      that is an input as well as an output. It is the Creation Kit's working
#      copy, and the build's sync_esp step has just reconciled it with
#      esp/plugin/;
#   4. copies the staging folder into the mod folder, skipping files that
#      are already identical there;
#   5. deletes from the mod folder whatever the previous build of this
#      checkout deployed and this one no longer stages, then records what it
#      staged in build/deployed-files.txt (outside build/<preset>, so `clean`
#      keeps it). The record is what makes the delete safe: the mod folder
#      also holds files this checkout never deployed -- another worktree's,
#      or whatever SkyrimNet writes there at runtime -- and those are left
#      alone.
#
# package.ps1 zips the staging folder, not the mod folder. The mod folder is
# shared by every worktree on the machine, and zipping it once shipped another
# worktree's dashboard bundle and language files in a release (v0.9.1).
#
# A build run straight from CMake (VS Code's CMake Tools, a bare
# `cmake --build`) skips steps 1 and 3-5: it refreshes the staging folder but
# deploys nothing to the game.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('configure', 'build', 'rebuild', 'test', 'clean')]
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
$stagingDir = Join-Path $buildDir 'staging'

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
    Clear-Staging
    Write-Host "==> cmake --build $buildDir" -ForegroundColor Cyan
    cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }
    Publish-Staging
}

# Empty the staging folder, so the build that follows starts from nothing.
# Ninja sees every staged file missing and re-runs each deploy rule.
function Clear-Staging {
    if (Test-Path -LiteralPath $stagingDir) {
        Remove-Item -LiteralPath $stagingDir -Recurse -Force
    }
}

# True when both files exist with identical contents.
function Test-SameFile([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $b -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $a).Length -ne (Get-Item -LiteralPath $b).Length) { return $false }
    return (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash -eq
    (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
}

# Stage the ESP, copy the staging folder into the mod folder, and delete what
# the previous build deployed that this one no longer stages. See the header.
function Publish-Staging {
    # Written by CMake only when it has somewhere to deploy to.
    $deployTargetFile = Join-Path $buildDir 'deploy-target.txt'
    if (-not (Test-Path -LiteralPath $deployTargetFile -PathType Leaf)) { return }
    $modFolder = [System.IO.Path]::GetFullPath((Get-Content -LiteralPath $deployTargetFile -Raw).Trim())

    if (-not (Test-Path -LiteralPath $stagingDir -PathType Container)) {
        Write-Warning "Staging folder $stagingDir was not created; nothing deployed."
        return
    }

    $modEsp = Join-Path $modFolder 'NarrativeEngine.esp'
    if (Test-Path -LiteralPath $modEsp -PathType Leaf) {
        Copy-Item -LiteralPath $modEsp -Destination (Join-Path $stagingDir 'NarrativeEngine.esp') -Force
    }

    # Case-insensitive, like the filesystem: a case-only rename must not read
    # as "old name gone", or we'd delete the file this build just deployed.
    $staged = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    Get-ChildItem -LiteralPath $stagingDir -Recurse -File -Force |
        ForEach-Object {
            [void]$staged.Add([System.IO.Path]::GetRelativePath($stagingDir, $_.FullName).Replace('\', '/'))
        }

    # An empty staging folder would make every recorded file look orphaned.
    if ($staged.Count -eq 0) {
        Write-Warning "Staging folder $stagingDir is empty; nothing deployed."
        return
    }

    $copied = 0
    foreach ($rel in $staged) {
        $src = Join-Path $stagingDir $rel
        $dst = Join-Path $modFolder $rel
        if (Test-SameFile $src $dst) { continue }
        New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $copied++
    }
    Write-Host "==> deployed $stagingDir -> $modFolder ($copied of $($staged.Count) file(s) changed)" -ForegroundColor Cyan

    # The record from before staging existed listed statics only; it seeds the
    # first run so files removed since then are still swept.
    $recordPath = Join-Path $PSScriptRoot 'build/deployed-files.txt'
    $legacyRecordPath = Join-Path $PSScriptRoot 'build/deployed-statics.txt'
    $previousRecord = $null
    if (Test-Path -LiteralPath $recordPath -PathType Leaf) {
        $previousRecord = $recordPath
    }
    elseif (Test-Path -LiteralPath $legacyRecordPath -PathType Leaf) {
        $previousRecord = $legacyRecordPath
    }

    $orphans = @()
    if ($previousRecord) {
        $orphans = @(
            Get-Content -LiteralPath $previousRecord |
                Where-Object { $_ -and -not $staged.Contains($_) } |
                ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $modFolder $_)) } |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
        )
    }

    if ($orphans.Count -gt 0) {
        Write-Host "==> pruning $($orphans.Count) file(s) this checkout no longer deploys from $modFolder" -ForegroundColor Cyan
        foreach ($orphan in $orphans) {
            Write-Host "    removed $([System.IO.Path]::GetRelativePath($modFolder, $orphan))" -ForegroundColor DarkGray
            Remove-Item -LiteralPath $orphan -Force

            # Remove the directories that deletion emptied, stopping at the mod folder.
            $dir = Split-Path -Parent $orphan
            while ($dir.Length -gt $modFolder.Length -and
                -not (Get-ChildItem -LiteralPath $dir -Force | Select-Object -First 1)) {
                Remove-Item -LiteralPath $dir -Force
                $dir = Split-Path -Parent $dir
            }
        }
    }

    Set-Content -LiteralPath $recordPath -Value ($staged | Sort-Object)
    if (Test-Path -LiteralPath $legacyRecordPath -PathType Leaf) {
        Remove-Item -LiteralPath $legacyRecordPath -Force
    }
}

# Build and run the Catch2 unit tests.
#
# Builds only the two test executables rather than the whole project, so the
# test loop doesn't pay for the ESP sync, the Papyrus compile, or the Rollup
# dashboard bundle — none of which any unit test can reach. ctest needs the
# Developer environment for the same reason cmake does, which is why this is a
# verb here instead of something you type by hand.
function Invoke-Test {
    if (-not (Test-Path "$buildDir/CMakeCache.txt")) {
        Write-Host "==> build dir not configured; running configure first" -ForegroundColor Yellow
        Invoke-Configure
    }
    $testTargets = @('NarrativeEngineTests', 'NarrativeEngineEngineMockTests')
    Write-Host "==> cmake --build $buildDir --target $($testTargets -join ' ')" -ForegroundColor Cyan
    cmake --build $buildDir --target @testTargets
    if ($LASTEXITCODE -ne 0) { throw "Test build failed (exit $LASTEXITCODE)." }

    Write-Host "==> ctest --test-dir $buildDir" -ForegroundColor Cyan
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed (exit $LASTEXITCODE)." }
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
    'test' { Invoke-Test }
    'clean' { Invoke-Clean }
}

Write-Host "==> done ($Verb / $Preset)" -ForegroundColor Green
