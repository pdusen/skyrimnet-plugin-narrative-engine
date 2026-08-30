# run-probe.ps1 — compile, link and run a probe inside the VS Developer
# environment.
#
# The compile-only sibling (compile-probe.ps1) verifies that something does or
# does not compile. This one verifies what code actually DOES: several steps of
# PHASE_14_FACTION_PLOTS.md turn on pure functions — the label derivation, the
# tick schedule, the progress-race arithmetic — and those need to run.
#
# Usage:
#   pwsh -File run-probe.ps1 <probe.cpp> [repo-relative-source.cpp ...]
#
# Exits with the probe's own exit code, so a probe that asserts can simply
# return non-zero.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Source,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraSources
)

$ErrorActionPreference = 'Stop'

$launchScript = Get-ChildItem `
    -Path 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' `
    -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $launchScript) {
    throw "Couldn't find Launch-VsDevShell.ps1 under 'C:\Program Files\Microsoft Visual Studio\2022\*'."
}

# Resolve before the dev shell moves the working directory.
$sourcePath = (Resolve-Path $Source).Path
$runner = Join-Path $PSScriptRoot 'run-probe.py'

& $launchScript.FullName -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$pyArgs = @($runner, $sourcePath)
if ($ExtraSources) { $pyArgs += $ExtraSources }

& python @pyArgs
exit $LASTEXITCODE
