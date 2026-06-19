# setup-mod-folder.ps1 — one-time per-machine setup for the ESP / Papyrus workflow.
#
# Creates the NarrativeEngine mod folder under $SKYRIM_MODS_FOLDER and the NTFS
# directory junction from <mod-folder>/Source/Scripts/ to <repo>/esp/Source/Scripts/.
# That junction is what lets CK (launched via MO2) and VS Code see the same .psc
# files — CK writes into Data/Source/Scripts/ (the virtualized mod-folder path),
# and those writes land directly in our repo's tracked source tree.
#
# Idempotent: safe to re-run. A second run prints a "already exists" line for
# each piece and exits cleanly.
#
# Run once per development machine after cloning; not part of any build.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

# --- env var checks ---------------------------------------------------------

$modsRoot = $env:SKYRIM_MODS_FOLDER
if (-not $modsRoot) {
    throw "SKYRIM_MODS_FOLDER environment variable is not set. Point it at your MO2 mods folder (e.g. C:/Modlists/<instance>/mods)."
}
if (-not (Test-Path $modsRoot -PathType Container)) {
    throw "SKYRIM_MODS_FOLDER ('$modsRoot') does not exist or is not a directory."
}

# --- create the mod folder if missing ---------------------------------------

$modFolder = Join-Path $modsRoot 'NarrativeEngine'
if (Test-Path $modFolder) {
    Write-Host "Mod folder already exists: $modFolder" -ForegroundColor DarkGray
}
else {
    New-Item -ItemType Directory -Path $modFolder | Out-Null
    Write-Host "Created mod folder: $modFolder" -ForegroundColor Green
}

# --- ensure the repo-side Papyrus source dir exists -------------------------
#
# The junction target must exist before New-Item -ItemType Junction will accept
# it as a target. On a fresh clone the dir exists because of the .gitkeep
# placeholder, but be defensive.

$repoSource = Join-Path $PSScriptRoot 'esp/Source/Scripts'
if (-not (Test-Path $repoSource)) {
    New-Item -ItemType Directory -Path $repoSource -Force | Out-Null
    Write-Host "Created repo source dir: $repoSource" -ForegroundColor Green
}

# --- create the directory junction ------------------------------------------
#
# Target: <mod-folder>/Source/Scripts/ -> <repo>/esp/Source/Scripts/
# Uses NTFS junctions (not file symlinks). Junctions don't require admin or
# Developer Mode and are handled transparently by all userspace software
# including MO2's USVFS.

$junctionParent = Join-Path $modFolder 'Source'
if (-not (Test-Path $junctionParent)) {
    New-Item -ItemType Directory -Path $junctionParent | Out-Null
}

$junctionPath = Join-Path $junctionParent 'Scripts'

if (Test-Path $junctionPath) {
    $item = Get-Item $junctionPath -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
        # Already a junction (or symlink). Trust it; don't second-guess the target.
        Write-Host "Junction already exists: $junctionPath" -ForegroundColor DarkGray
    }
    else {
        # Regular directory. Only safe to replace if it's empty.
        $children = Get-ChildItem $junctionPath -Force -ErrorAction SilentlyContinue
        if (-not $children) {
            Remove-Item $junctionPath -Force
            New-Item -ItemType Junction -Path $junctionPath -Target $repoSource | Out-Null
            Write-Host "Replaced empty directory with junction: $junctionPath -> $repoSource" -ForegroundColor Green
        }
        else {
            throw "$junctionPath exists as a regular (non-junction) directory and is not empty. Move its contents into <repo>/esp/Source/Scripts/ manually, delete $junctionPath, and re-run this script."
        }
    }
}
else {
    New-Item -ItemType Junction -Path $junctionPath -Target $repoSource | Out-Null
    Write-Host "Created junction: $junctionPath -> $repoSource" -ForegroundColor Green
}

# --- sanity-check PAPYRUS_COMPILER (warn-only) ------------------------------
#
# Needed by the CMake Papyrus compile step once .psc files exist. Not needed
# for Step 1's verify; warn so the user knows to set it before Step 7.

if (-not $env:PAPYRUS_COMPILER) {
    Write-Warning "PAPYRUS_COMPILER environment variable is not set. You'll need it before the first .psc file exists (Step 7). Typical path: <CK_DIR>/Papyrus Compiler/PapyrusCompiler.exe"
}
elseif (-not (Test-Path $env:PAPYRUS_COMPILER -PathType Leaf)) {
    Write-Warning "PAPYRUS_COMPILER ('$($env:PAPYRUS_COMPILER)') does not point at an existing file."
}
else {
    Write-Host "PAPYRUS_COMPILER: $($env:PAPYRUS_COMPILER)" -ForegroundColor DarkGray
}

Write-Host "Setup complete." -ForegroundColor Cyan
