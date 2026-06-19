# sync-esp.ps1 — copy NarrativeEngine.esp from the mod folder to the repo
# when the mod folder's copy is newer. One direction only: mod folder -> repo.
# The repo never pushes the ESP back to the mod folder.
#
# Invoked automatically by CMake's `sync_esp` ALL target on every build, so
# CK edits made between builds get pulled into the repo before anything else
# runs. Also safe to run standalone for diagnostics — pass -DryRun for a
# verbose decision trace without writing anything.
#
# Decision matrix:
#   neither exists           → silent no-op (pre-Step-2 state)
#   only mod exists          → first-time sync (mod -> repo)
#   only repo exists         → silent no-op (deploy is the user's manual job;
#                              this is rare in solo development)
#   both exist, mod newer    → sync (mod -> repo)
#   both exist, mod ≤ repo   → silent no-op
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

$modsRoot = $env:SKYRIM_MODS_FOLDER
if (-not $modsRoot) {
    Write-NoOp "no action — SKYRIM_MODS_FOLDER is not set."
    return
}

$modEsp  = Join-Path $modsRoot 'NarrativeEngine/NarrativeEngine.esp'
$repoEsp = Join-Path $PSScriptRoot 'esp/NarrativeEngine.esp'

$modExists  = Test-Path $modEsp  -PathType Leaf
$repoExists = Test-Path $repoEsp -PathType Leaf

if (-not $modExists -and -not $repoExists) {
    Write-NoOp "no action — neither file exists."
    return
}

if ($modExists -and -not $repoExists) {
    Write-Host "ESP: first-time sync from mod folder" -ForegroundColor Green
    if (-not $DryRun) {
        $repoDir = Split-Path $repoEsp -Parent
        if (-not (Test-Path $repoDir)) {
            New-Item -ItemType Directory -Path $repoDir -Force | Out-Null
        }
        Copy-Item -Path $modEsp -Destination $repoEsp -Force
    }
    return
}

if (-not $modExists -and $repoExists) {
    Write-NoOp "no action — only repo ESP exists; deploy is the user's manual responsibility."
    return
}

# Both exist — compare timestamps.
$modTime  = (Get-Item $modEsp).LastWriteTime
$repoTime = (Get-Item $repoEsp).LastWriteTime

if ($modTime -gt $repoTime) {
    $deltaSec = [int]($modTime - $repoTime).TotalSeconds
    Write-Host "ESP: synced from mod folder (CK edits detected, deployed ${deltaSec}s newer)" -ForegroundColor Green
    if (-not $DryRun) {
        Copy-Item -Path $modEsp -Destination $repoEsp -Force
    }
}
else {
    Write-NoOp "no action — repo ESP is up to date (mod=$modTime, repo=$repoTime)."
}
