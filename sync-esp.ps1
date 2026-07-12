# sync-esp.ps1 — bidirectional sync of NarrativeEngine.esp between the
# repo's authoritative path and the deployed mod folder. Whichever copy is
# newer wins; the older one is overwritten.
#
# Invoked automatically by CMake's `sync_esp` ALL target on every build, so
# CK edits made between builds get pulled into the repo AND repo changes
# (e.g. after a `git pull`) get pushed back out to the mod folder before
# anything else runs. Also safe to run standalone for diagnostics — pass
# -DryRun for a verbose decision trace without writing anything.
#
# Decision matrix:
#   neither exists           → silent no-op (pre-Step-2 state)
#   only mod exists          → mod → repo (first-time sync into repo)
#   only repo exists         → repo → mod (first-time deploy to mod folder)
#   both exist, mod newer    → mod → repo
#   both exist, repo newer   → repo → mod
#   both exist, equal mtime  → silent no-op
#
# In DryRun mode the script always prints its decision (including no-op
# cases), and never copies anything.

[CmdletBinding()]
param(
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Write-NoOp([string]$message) {
    if ($DryRun) {
        Write-Host $message -ForegroundColor DarkGray
    }
}

function Copy-Esp([string]$src, [string]$dst, [string]$label) {
    Write-Host $label -ForegroundColor Green
    if ($DryRun) { return }
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path $dstDir)) {
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
    }
    Copy-Item -Path $src -Destination $dst -Force
}

$modsRoot = $env:SKYRIM_MODS_FOLDER
if (-not $modsRoot) {
    Write-NoOp "no action — SKYRIM_MODS_FOLDER is not set."
    return
}

$modEsp = Join-Path $modsRoot 'NarrativeEngine/NarrativeEngine.esp'
$repoEsp = Join-Path $PSScriptRoot 'esp/NarrativeEngine.esp'

$modExists = Test-Path $modEsp  -PathType Leaf
$repoExists = Test-Path $repoEsp -PathType Leaf

if (-not $modExists -and -not $repoExists) {
    Write-NoOp "no action — neither file exists."
    return
}

if ($modExists -and -not $repoExists) {
    Copy-Esp $modEsp $repoEsp "ESP: first-time sync from mod folder to repo"
    return
}

if (-not $modExists -and $repoExists) {
    Copy-Esp $repoEsp $modEsp "ESP: first-time deploy from repo to mod folder"
    return
}

# Both exist — compare timestamps.
$modTime = (Get-Item $modEsp).LastWriteTime
$repoTime = (Get-Item $repoEsp).LastWriteTime

if ($modTime -gt $repoTime) {
    $deltaSec = [int]($modTime - $repoTime).TotalSeconds
    Copy-Esp $modEsp $repoEsp "ESP: mod folder → repo (CK edits detected, mod folder ${deltaSec}s newer)"
}
elseif ($repoTime -gt $modTime) {
    $deltaSec = [int]($repoTime - $modTime).TotalSeconds
    Copy-Esp $repoEsp $modEsp "ESP: repo → mod folder (repo ${deltaSec}s newer)"
}
else {
    Write-NoOp "no action — both copies have equal mtime (mod=$modTime, repo=$repoTime)."
}
