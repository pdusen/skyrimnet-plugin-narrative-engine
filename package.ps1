# package.ps1 — build + zip the deployed mod folder into an MO2-compatible archive.
#
# Runs build.ps1 first so the mod folder under $SKYRIM_MODS_FOLDER/NarrativeEngine/
# reflects the current source (compiled DLL, dashboard bundle, statics deploy,
# compiled Papyrus, ESP sync — everything build.ps1 wires up). Then packages the
# folder's *contents* (not the folder itself) into a zip so the archive's top-
# level entries are what MO2 / Vortex expect at the root of a direct install
# (SKSE/, PrismaUI/, MCM/, Scripts/, NarrativeEngine.esp, etc.).
#
# Usage:
#   pwsh -File package.ps1 -Version 0.0.1            # writes out/NarrativeEngine-v0.0.1.zip
#   pwsh -File package.ps1 -Version 0.0.1 -OutputDir C:\Releases
#
# The -Version argument is a bare semver-ish string. A leading 'v' is
# stripped defensively; the filename always uses the canonical
# 'NarrativeEngine-v<Version>.zip' shape.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,

    [string]$OutputDir = (Join-Path $PSScriptRoot 'out')
)

$ErrorActionPreference = 'Stop'

# Normalize the version — accept '0.0.1', 'v0.0.1', 'V0.0.1' identically.
$Version = $Version -replace '^[vV]', ''

# The mod folder lives under SKYRIM_MODS_FOLDER, deployed there by CMake's
# post-build steps invoked via build.ps1. Missing env var means we don't know
# where to look; see docs/DEVELOPMENT.md.
$modsFolder = $env:SKYRIM_MODS_FOLDER
if (-not $modsFolder) {
    throw "SKYRIM_MODS_FOLDER is not set. See docs/DEVELOPMENT.md for setup."
}
$modFolder = Join-Path $modsFolder 'NarrativeEngine'

# --- 1. Build first ---------------------------------------------------------
#
# Any build failure aborts here — packaging a stale or partial mod folder
# would produce a broken archive.

Write-Host "==> Running build.ps1 to sync deployed mod content" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'build.ps1') build
if ($LASTEXITCODE -ne 0) {
    throw "build.ps1 failed (exit $LASTEXITCODE); refusing to package."
}

# --- 2. Validate ESL FormID range in the synced plugin tree ----------------
#
# build.ps1 triggers CMake's sync_esp target, which runs sync-esp.ps1 and refreshes
# esp/plugin/ from the mod folder. That refresh is what we validate here - any CK
# session that assigned an out-of-ESL-range FormID (a recurring CK bug on light-flagged
# plugins) will have been captured in the fresh YAML tree, and this check surfaces it
# BEFORE we bake the broken ESP into a release archive.

Write-Host '==> Validating ESL FormID range in esp/plugin/' -ForegroundColor Cyan
& (Join-Path $PSScriptRoot 'check-esl-formids.ps1')
if ($LASTEXITCODE -ne 0) {
    throw "check-esl-formids.ps1 reported violations (exit $LASTEXITCODE); refusing to package."
}

# --- 3. Sanity-check the mod folder ----------------------------------------

if (-not (Test-Path -LiteralPath $modFolder -PathType Container)) {
    throw "Expected mod folder does not exist: $modFolder"
}
$modContents = Get-ChildItem -LiteralPath $modFolder -Force
if (-not $modContents) {
    throw "Mod folder is empty: $modFolder"
}

# --- 4. Package -------------------------------------------------------------
#
# CreateFromDirectory with includeBaseDirectory=$false puts the folder's
# *contents* at the archive root — exactly what MO2 / Vortex expect for a
# direct-install archive. Compress-Archive would also work but is markedly
# slower on trees of this size and handles edge cases (empty directories,
# long paths) less reliably.

$archiveName = "NarrativeEngine-v$Version.zip"
$archivePath = Join-Path $OutputDir $archiveName

if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

Write-Host "==> Packaging $modFolder -> $archivePath" -ForegroundColor Cyan

Add-Type -AssemblyName 'System.IO.Compression.FileSystem'
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $modFolder,
    $archivePath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)

$archiveSize = (Get-Item -LiteralPath $archivePath).Length
Write-Host ("==> Done: {0} ({1:N1} MB)" -f $archivePath, ($archiveSize / 1MB)) -ForegroundColor Green
