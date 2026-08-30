# compile-probe.ps1 — run a compile probe inside the VS Developer environment.
#
# The token discipline in this plugin is enforced by the compiler, so the way
# to verify it is to compile something that must NOT compile and confirm the
# specific diagnostic. compile-probe.py does that against the real build flags;
# this wrapper supplies the toolchain environment it needs, exactly as
# build.ps1 does for the build itself.
#
# Usage:
#   pwsh -File compile-probe.ps1 <probe.cpp> pass
#   pwsh -File compile-probe.ps1 <probe.cpp> fail C2672
#
# Probe .cpp files are throwaway. Write one, run it, record the result in the
# phase doc, delete it. This script and compile-probe.py are the parts worth
# keeping — several steps of PHASE_14_FACTION_PLOTS.md need them.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Source,

    [Parameter(Mandatory = $true, Position = 1)]
    [ValidateSet('pass', 'fail')]
    [string]$Expect,

    [Parameter(Position = 2)]
    [string]$ExpectedErrorCode
)

$ErrorActionPreference = 'Stop'

$launchScript = Get-ChildItem `
    -Path 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\Launch-VsDevShell.ps1' `
    -ErrorAction SilentlyContinue |
    Select-Object -First 1

if (-not $launchScript) {
    throw "Couldn't find Launch-VsDevShell.ps1 under 'C:\Program Files\Microsoft Visual Studio\2022\*'."
}

# Resolve before the dev shell changes the working directory out from under us.
$sourcePath = (Resolve-Path $Source).Path
$runner = Join-Path $PSScriptRoot 'compile-probe.py'

& $launchScript.FullName -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$pyArgs = @($runner, $sourcePath, $Expect)
if ($ExpectedErrorCode) { $pyArgs += $ExpectedErrorCode }

& python @pyArgs
exit $LASTEXITCODE
