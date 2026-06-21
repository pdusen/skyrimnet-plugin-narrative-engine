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

# --- install git pre-commit hook --------------------------------------------
#
# Runs sync-esp.ps1 before every commit. If the sync updates
# esp/NarrativeEngine.esp (because the mod-folder copy was newer), the hook
# also stages the updated file so the change rides along in the same commit
# the developer was already making — no risk of forgetting to commit the
# latest CK edits.
#
# The hook is written with LF line endings so git-bash (which executes hooks
# on Windows) can run it cleanly. Detection of "is this hook ours" is by a
# marker comment in the script body; we replace our own version freely and
# leave any third-party hook alone with a warning.

$gitDir = Join-Path $PSScriptRoot '.git'
if (Test-Path $gitDir -PathType Container) {
    $hooksDir = Join-Path $gitDir 'hooks'
    if (-not (Test-Path $hooksDir)) {
        New-Item -ItemType Directory -Path $hooksDir | Out-Null
    }
    $hookPath = Join-Path $hooksDir 'pre-commit'
    $marker   = '# NarrativeEngine pre-commit hook — managed by setup-mod-folder.ps1'

    # Hook body. Escape $ as `$ inside the here-string so PowerShell doesn't
    # interpolate the shell variables.
    $hookBody = @"
#!/bin/sh
$marker
# Runs sync-esp.ps1 so the repo's ESP matches the mod folder, and stages any
# resulting ESP change so it's part of this commit.

REPO_ROOT="`$(git rev-parse --show-toplevel)"
ESP_PATH="`$REPO_ROOT/esp/NarrativeEngine.esp"

# Hash before — `git hash-object` returns the same hash git would store, so
# it's exactly the right shape for "did the file content change."
BEFORE_HASH=`$(git hash-object "`$ESP_PATH" 2>/dev/null || echo "")

if ! pwsh -NoProfile -File "`$REPO_ROOT/sync-esp.ps1"; then
    echo "pre-commit: sync-esp.ps1 failed; aborting commit" >&2
    exit 1
fi

AFTER_HASH=`$(git hash-object "`$ESP_PATH" 2>/dev/null || echo "")
if [ "`$BEFORE_HASH" != "`$AFTER_HASH" ] && [ -f "`$ESP_PATH" ]; then
    echo "pre-commit: sync-esp.ps1 updated esp/NarrativeEngine.esp; staging the change"
    git add -- "`$ESP_PATH"
fi
"@

    $shouldInstall = $false
    $action        = $null
    if (-not (Test-Path $hookPath)) {
        $shouldInstall = $true
        $action        = 'Installed'
    }
    else {
        $existing = Get-Content $hookPath -Raw -ErrorAction SilentlyContinue
        if ($existing -and $existing.Contains($marker)) {
            $shouldInstall = $true
            $action        = 'Updated'
        }
        else {
            Write-Warning "$hookPath exists and was not installed by this script; leaving it alone. Merge the sync-esp behavior in manually if you want it."
        }
    }

    if ($shouldInstall) {
        # Normalize to LF so git-bash executes the hook on Windows.
        $lfBody = $hookBody -replace "`r`n", "`n"
        [System.IO.File]::WriteAllText($hookPath, $lfBody)
        Write-Host "$action pre-commit hook: $hookPath" -ForegroundColor Green
    }
}
else {
    Write-Host "No .git directory found at $gitDir; skipping pre-commit hook install" -ForegroundColor DarkGray
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
