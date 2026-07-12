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

# --- copy SkyrimNetApi.psc into external/ ------------------------------------
#
# Copies <SkyrimNet mod folder>/Source/Scripts/SkyrimNetApi.psc into
# <repo>/external/SkyrimNet/Source/Scripts/SkyrimNetApi.psc so the Papyrus
# compiler (invoked directly by CMake, not through MO2's VFS) can resolve
# `SkyrimNetApi` as an import. VS Code's Papyrus extension picks it up
# through the same directory via the generated .ppj file.
#
# A plain copy (rather than a file symlink) avoids the Windows admin/Developer
# Mode requirement, and the copy is refreshed on every setup run so we track
# any SkyrimNet update. If someone left an old symlink from a previous run, we
# replace it with a real file.
#
# Location resolution matches CMakeLists.txt: honor $env:SKYRIMNET_DIR if
# set, otherwise fall back to <mods-folder>/SkyrimNet.

$skyrimNetRoot = $env:SKYRIMNET_DIR
if (-not $skyrimNetRoot) {
    $skyrimNetRoot = Join-Path $modsRoot 'SkyrimNet'
}

$skyrimNetApiSource = Join-Path $skyrimNetRoot 'Source/Scripts/SkyrimNetApi.psc'
if (-not (Test-Path $skyrimNetApiSource -PathType Leaf)) {
    Write-Warning "SkyrimNetApi.psc not found at '$skyrimNetApiSource'. Install SkyrimNet or set SKYRIMNET_DIR, then re-run this script to copy the import file."
}
else {
    # The containing dir must exist as a real directory — this is what CMake
    # passes to PapyrusCompiler as an -i= import path.
    $skyrimNetCopyDir = Join-Path $PSScriptRoot 'external/SkyrimNet/Source/Scripts'
    if (-not (Test-Path $skyrimNetCopyDir)) {
        New-Item -ItemType Directory -Path $skyrimNetCopyDir -Force | Out-Null
    }

    $skyrimNetApiCopy = Join-Path $skyrimNetCopyDir 'SkyrimNetApi.psc'

    # If a stale reparse point (symlink from earlier versions of this script)
    # sits at the destination, drop it first — Copy-Item into a symlink writes
    # through the link rather than replacing it.
    if (Test-Path $skyrimNetApiCopy) {
        $existing = Get-Item $skyrimNetApiCopy -Force
        if ($existing.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            Remove-Item $skyrimNetApiCopy -Force
            Write-Host "Removed stale SkyrimNetApi.psc symlink: $skyrimNetApiCopy" -ForegroundColor DarkGray
        }
    }

    Copy-Item -Path $skyrimNetApiSource -Destination $skyrimNetApiCopy -Force
    Write-Host "Copied SkyrimNetApi.psc: $skyrimNetApiSource -> $skyrimNetApiCopy" -ForegroundColor Green
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
# on Windows) can run it cleanly. Any existing pre-commit hook is overwritten
# unconditionally — this script owns that file.

$gitDir = Join-Path $PSScriptRoot '.git'
if (Test-Path $gitDir -PathType Container) {
    $hooksDir = Join-Path $gitDir 'hooks'
    if (-not (Test-Path $hooksDir)) {
        New-Item -ItemType Directory -Path $hooksDir | Out-Null
    }
    $hookPath = Join-Path $hooksDir 'pre-commit'

    # Hook body. Escape $ as `$ inside the here-string so PowerShell doesn't
    # interpolate the shell variables.
    $hookBody = @"
#!/bin/sh
# NarrativeEngine pre-commit hook — managed by setup-mod-folder.ps1
# Runs sync-esp.ps1 so the repo's ESP matches the mod folder, stages any
# resulting ESP change, then runs `pre-commit run` against staged files so
# the formatters/linters in .pre-commit-config.yaml gate the commit.

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

# Run the pre-commit framework against staged files. Prefer the `pre-commit`
# command on PATH; fall back to `python -m pre_commit` so pip --user installs
# work before a fresh shell has picked up the new Scripts/ dir on PATH.
# Skipped silently if neither resolves so a fresh clone can still commit before
# setup is complete; the README's 'Linting and autoformatting' section walks
# through the install.
PRE_COMMIT_CMD=""
if command -v pre-commit >/dev/null 2>&1; then
    PRE_COMMIT_CMD="pre-commit"
elif command -v python >/dev/null 2>&1 && python -c "import pre_commit" >/dev/null 2>&1; then
    PRE_COMMIT_CMD="python -m pre_commit"
elif command -v py >/dev/null 2>&1 && py -c "import pre_commit" >/dev/null 2>&1; then
    PRE_COMMIT_CMD="py -m pre_commit"
fi

if [ -n "`$PRE_COMMIT_CMD" ]; then
    # Capture output so VS Code's Git integration doesn't pop a "Git Hooks"
    # dialog for the happy path (any stderr output triggers it, even on
    # success). On failure, replay the full log to stderr so the developer
    # sees exactly what went wrong.
    PRE_COMMIT_OUT=`$(`$PRE_COMMIT_CMD run --hook-stage pre-commit 2>&1)
    PRE_COMMIT_STATUS=`$?
    if [ `$PRE_COMMIT_STATUS -ne 0 ]; then
        echo "`$PRE_COMMIT_OUT" >&2
        echo "" >&2
        echo "pre-commit: formatters/linters reported issues; aborting commit" >&2
        echo "pre-commit: run 'pwsh -File format.ps1' to auto-fix, then re-stage and commit" >&2
        exit 1
    fi
else
    echo "pre-commit: not on PATH and Python can't import pre_commit; skipping formatter/linter stage" >&2
    echo "pre-commit: see README 'Linting and autoformatting' to install" >&2
fi
"@

    $action = if (Test-Path $hookPath) { 'Replaced' } else { 'Installed' }

    # Normalize to LF so git-bash executes the hook on Windows.
    $lfBody = $hookBody -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($hookPath, $lfBody)
    Write-Host "$action pre-commit hook: $hookPath" -ForegroundColor Green
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
