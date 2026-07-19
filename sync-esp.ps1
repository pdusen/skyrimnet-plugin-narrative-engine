# sync-esp.ps1 — bidirectional sync of NarrativeEngine.esp between the
# deployed mod folder and the repo's serialized YAML tree under esp/plugin/.
#
# The mod-folder side is a single binary <mod-folder>/NarrativeEngine.esp; the
# repo side is a Spriggit-serialized directory tree at <repo>/esp/plugin/.
# Spriggit is invoked at each boundary:
#   mod -> repo:  Spriggit.CLI.exe serialize   -i <mod-esp>       -o esp/plugin
#   repo -> mod:  Spriggit.CLI.exe deserialize -i esp/plugin      -o <mod-esp>
#
# Because serialize/deserialize is not a byte-preserving round-trip, we can't
# compare mtimes across the boundary. Instead we track content hashes in a
# machine-local state file (esp/.sync-state.json, gitignored) that records the
# mod-esp SHA-256 and the plugin-tree SHA-256 at the last successful sync. On
# each run we recompute both, compare to the stored baseline, and:
#
#   Steady-state (state file present):
#     neither changed       -> no-op
#     only mod changed      -> serialize   mod -> repo, rewrite state
#     only repo changed     -> deserialize repo -> mod, rewrite state
#     both changed          -> divergence; refuse unless -Prefer is passed
#
#   Bootstrap (state file absent, e.g. fresh clone / migration):
#     neither exists        -> no-op
#     only mod exists       -> first-time serialize mod -> repo
#     only repo exists      -> first-time deserialize repo -> mod
#     both exist            -> tiebreak on mtime (mod-esp mtime vs. newest
#                              file under esp/plugin/); newer side wins.
#                              Overridable with -Prefer mod|repo.
#
# -DryRun prints the decision without touching either side or the state file.
# -Prefer mod|repo forces one direction in ambiguous (divergent / bootstrap-
# both-exist) cases; ignored when there's no ambiguity.
#
# Invoked automatically by CMake's `sync_esp` ALL target on every build, and by
# the pre-commit hook installed by setup-mod-folder.ps1. Also safe standalone.

[CmdletBinding()]
param(
    [switch]$DryRun,
    [ValidateSet('mod', 'repo')]
    [string]$Prefer
)

$ErrorActionPreference = 'Stop'

# --- paths ------------------------------------------------------------------

$modsRoot = $env:SKYRIM_MODS_FOLDER
if (-not $modsRoot) {
    if ($DryRun) {
        Write-Host 'no action - SKYRIM_MODS_FOLDER is not set.' -ForegroundColor DarkGray
    }
    return
}

$modEsp = Join-Path $modsRoot 'NarrativeEngine/NarrativeEngine.esp'
$repoPluginDir = Join-Path $PSScriptRoot 'esp/plugin'
$stateFile = Join-Path $PSScriptRoot 'esp/.sync-state.json'

# --- Spriggit CLI locator ---------------------------------------------------
#
# Honor $env:SPRIGGIT_CLI as an explicit override, otherwise fall back to
# whatever `spriggit` / `Spriggit.CLI.exe` is on PATH. Fatal if neither
# resolves - we can't sync without it.

function Resolve-SpriggitCli {
    if ($env:SPRIGGIT_CLI -and (Test-Path -LiteralPath $env:SPRIGGIT_CLI -PathType Leaf)) {
        return $env:SPRIGGIT_CLI
    }
    foreach ($name in @('Spriggit.CLI.exe', 'Spriggit.CLI', 'spriggit')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($cmd) { return $cmd.Source }
    }
    throw 'Spriggit.CLI.exe not found on PATH. Install Spriggit or set $env:SPRIGGIT_CLI to its full path.'
}

# --- hashing helpers --------------------------------------------------------
#
# Get-PluginTreeHash produces a single SHA-256 over the serialized YAML tree,
# folding every file's relative path AND content into the hash so a rename
# alone is a change. Files are visited in sorted-relpath order for
# determinism across filesystems. Result is lowercase hex, or $null if the
# folder doesn't exist / is empty.

function Get-FileHashHex([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-PluginTreeHash([string]$folder) {
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { return $null }
    $files = Get-ChildItem -LiteralPath $folder -Recurse -File -Force |
        Where-Object { $_.Name -ne '.gitkeep' } |
        Sort-Object { $_.FullName.Substring($folder.Length).TrimStart('\', '/').Replace('\', '/') }
    if (-not $files) { return $null }

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $ms = New-Object System.IO.MemoryStream
        try {
            foreach ($f in $files) {
                $rel = $f.FullName.Substring($folder.Length).TrimStart('\', '/').Replace('\', '/')
                $relBytes = [System.Text.Encoding]::UTF8.GetBytes($rel + [char]0)
                $ms.Write($relBytes, 0, $relBytes.Length)
                $contentBytes = [System.IO.File]::ReadAllBytes($f.FullName)
                $contentHash = $sha.ComputeHash($contentBytes)
                $ms.Write($contentHash, 0, $contentHash.Length)
            }
            $ms.Position = 0
            $treeBytes = $ms.ToArray()
            $treeHash = $sha.ComputeHash($treeBytes)
            return ([BitConverter]::ToString($treeHash) -replace '-', '').ToLowerInvariant()
        }
        finally { $ms.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-NewestPluginTreeMtime([string]$folder) {
    if (-not (Test-Path -LiteralPath $folder -PathType Container)) { return $null }
    $files = Get-ChildItem -LiteralPath $folder -Recurse -File -Force |
        Where-Object { $_.Name -ne '.gitkeep' }
    if (-not $files) { return $null }
    return ($files | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime
}

# --- state file I/O ---------------------------------------------------------

function Read-SyncState {
    if (-not (Test-Path -LiteralPath $stateFile -PathType Leaf)) { return $null }
    try {
        return Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    }
    catch {
        Write-Warning "Failed to parse $stateFile ($_). Treating as absent."
        return $null
    }
}

function Write-SyncState([string]$modHash, [string]$treeHash) {
    if ($DryRun) { return }
    $stateDir = [System.IO.Path]::GetDirectoryName($stateFile)
    if (-not (Test-Path -LiteralPath $stateDir)) {
        New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
    }
    $obj = [ordered]@{
        modEspSha256     = $modHash
        pluginTreeSha256 = $treeHash
        updatedAt        = (Get-Date).ToUniversalTime().ToString('o')
    }
    ($obj | ConvertTo-Json) | Set-Content -LiteralPath $stateFile -Encoding UTF8
}

# --- Spriggit invocations ---------------------------------------------------

function Invoke-Spriggit([string]$verb, [string]$inputPath, [string]$outputPath) {
    $cli = Resolve-SpriggitCli
    Write-Host "  running: $cli $verb -i `"$inputPath`" -o `"$outputPath`"" -ForegroundColor DarkGray
    # Use [IO.Path]::GetDirectoryName instead of `Split-Path -LiteralPath -Parent` — the
    # `-LiteralPath` and `-Parent` parameters live in different Split-Path parameter sets and
    # can't be combined on some PowerShell 7 builds, which raises "Parameter set cannot be
    # resolved" and skips the Spriggit invocation.
    $outDir = if ($verb -eq 'serialize') { $outputPath } else { [System.IO.Path]::GetDirectoryName($outputPath) }
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    }
    & $cli $verb -i $inputPath -o $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "Spriggit $verb failed (exit $LASTEXITCODE)."
    }
}

function Invoke-SerializeModToRepo([string]$reason) {
    Write-Host "ESP: mod folder -> repo (serialize) [$reason]" -ForegroundColor Green
    if (-not $DryRun) {
        Invoke-Spriggit 'serialize' $modEsp $repoPluginDir
        $newModHash = Get-FileHashHex $modEsp
        $newTreeHash = Get-PluginTreeHash $repoPluginDir
        Write-SyncState $newModHash $newTreeHash
    }
}

function Invoke-DeserializeRepoToMod([string]$reason) {
    Write-Host "ESP: repo -> mod folder (deserialize) [$reason]" -ForegroundColor Green
    if (-not $DryRun) {
        Invoke-Spriggit 'deserialize' $repoPluginDir $modEsp
        $newModHash = Get-FileHashHex $modEsp
        $newTreeHash = Get-PluginTreeHash $repoPluginDir
        Write-SyncState $newModHash $newTreeHash
    }
}

# --- current-state snapshot -------------------------------------------------

$modExists = Test-Path -LiteralPath $modEsp -PathType Leaf
$repoExists = (Test-Path -LiteralPath $repoPluginDir -PathType Container) -and
($null -ne (Get-PluginTreeHash $repoPluginDir))

$currentModHash = if ($modExists) { Get-FileHashHex $modEsp } else { $null }
$currentTreeHash = if ($repoExists) { Get-PluginTreeHash $repoPluginDir } else { $null }

$state = Read-SyncState

# --- bootstrap: no state file yet -------------------------------------------

if (-not $state) {
    if (-not $modExists -and -not $repoExists) {
        if ($DryRun) { Write-Host 'no action - neither side exists and no state file.' -ForegroundColor DarkGray }
        return
    }
    if ($modExists -and -not $repoExists) {
        Invoke-SerializeModToRepo 'bootstrap: only mod-folder ESP exists'
        return
    }
    if (-not $modExists -and $repoExists) {
        Invoke-DeserializeRepoToMod 'bootstrap: only repo plugin tree exists'
        return
    }

    # Both sides exist and we have no baseline. Prefer explicit -Prefer, else
    # tiebreak on mtime (same shape as the old script's "newest wins" rule).
    if ($Prefer -eq 'mod') {
        Invoke-SerializeModToRepo 'bootstrap: -Prefer mod'
        return
    }
    if ($Prefer -eq 'repo') {
        Invoke-DeserializeRepoToMod 'bootstrap: -Prefer repo'
        return
    }

    $modMtime = (Get-Item -LiteralPath $modEsp).LastWriteTime
    $treeMtime = Get-NewestPluginTreeMtime $repoPluginDir
    if ($modMtime -gt $treeMtime) {
        $deltaSec = [int]($modMtime - $treeMtime).TotalSeconds
        Invoke-SerializeModToRepo "bootstrap: both exist, mod ${deltaSec}s newer by mtime"
    }
    elseif ($treeMtime -gt $modMtime) {
        $deltaSec = [int]($treeMtime - $modMtime).TotalSeconds
        Invoke-DeserializeRepoToMod "bootstrap: both exist, repo ${deltaSec}s newer by mtime"
    }
    else {
        # Exactly equal mtimes at bootstrap is extraordinarily unlikely (would
        # mean the tree was synthesized to match the ESP's mtime). Treat as
        # "already in sync" - just record the state.
        Write-Host 'ESP: bootstrap no-op - both sides exist with equal mtime; recording baseline.' -ForegroundColor DarkGray
        if (-not $DryRun) { Write-SyncState $currentModHash $currentTreeHash }
    }
    return
}

# --- steady-state: state file present ---------------------------------------
#
# "Changed" means the current hash differs from the recorded baseline, OR the
# side has vanished / appeared since the last sync. A vanished side that's
# not going to come back on its own is a real change to handle.

$modChanged = ($currentModHash -ne $state.modEspSha256)
$repoChanged = ($currentTreeHash -ne $state.pluginTreeSha256)

if (-not $modChanged -and -not $repoChanged) {
    if ($DryRun) { Write-Host "no action - both sides match state baseline." -ForegroundColor DarkGray }
    return
}

if ($modChanged -and -not $repoChanged) {
    if (-not $modExists) {
        # Mod ESP disappeared. Deserialize to restore it from repo.
        Invoke-DeserializeRepoToMod 'mod ESP missing; restoring from repo'
        return
    }
    Invoke-SerializeModToRepo 'CK edits detected in mod folder'
    return
}

if ($repoChanged -and -not $modChanged) {
    if (-not $repoExists) {
        Write-Warning 'Plugin tree is missing but mod ESP is unchanged since last sync - refusing to serialize (would lose committed history). Restore esp/plugin/ from git.'
        return
    }
    Invoke-DeserializeRepoToMod 'repo plugin tree changed (pull or manual edit)'
    return
}

# Both changed - divergence.
if ($Prefer -eq 'mod') {
    Invoke-SerializeModToRepo 'divergence: -Prefer mod'
    return
}
if ($Prefer -eq 'repo') {
    Invoke-DeserializeRepoToMod 'divergence: -Prefer repo'
    return
}

$msg = @(
    'ESP sync: BOTH sides have changed since the last successful sync.'
    "  mod-folder ESP hash: $currentModHash (baseline was $($state.modEspSha256))"
    "  repo plugin tree hash: $currentTreeHash (baseline was $($state.pluginTreeSha256))"
    'Refusing to auto-resolve - one side would clobber real work on the other.'
    'Re-run with -Prefer mod (keep CK edits, overwrite repo) or'
    '           -Prefer repo (keep repo state, overwrite mod folder) to resolve.'
) -join [Environment]::NewLine
throw $msg
