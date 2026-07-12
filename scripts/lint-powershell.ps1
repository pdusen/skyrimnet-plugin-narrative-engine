# scripts/lint-powershell.ps1 — invoked by the pre-commit `powershell-format`
# hook against one or more .ps1/.psm1/.psd1 files. Runs Invoke-Formatter to
# rewrite each file in place, then Invoke-ScriptAnalyzer to fail the hook on
# Warning-or-higher findings so they can't sneak into a commit.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Module -ListAvailable -Name PSScriptAnalyzer)) {
    Write-Error "PSScriptAnalyzer module is not installed. Run: Install-Module PSScriptAnalyzer -Scope CurrentUser"
    exit 1
}

Import-Module PSScriptAnalyzer -ErrorAction Stop

# Repo-level rule exclusions live in PSScriptAnalyzerSettings.psd1 at the repo
# root. Locate it relative to this script so `pre-commit run` (which cd's to
# the repo root) and direct invocations both find it.
$settingsFile = Join-Path $PSScriptRoot '..' 'PSScriptAnalyzerSettings.psd1'
$settingsFile = [System.IO.Path]::GetFullPath($settingsFile)

$hadFindings = $false

foreach ($file in $Files) {
    if (-not (Test-Path $file -PathType Leaf)) {
        continue
    }

    $original = Get-Content -Raw -Path $file
    $formatted = Invoke-Formatter -ScriptDefinition $original
    if ($formatted -ne $original) {
        Set-Content -Path $file -Value $formatted -NoNewline
        Write-Host "reformatted: $file" -ForegroundColor Yellow
    }

    $analyzerArgs = @{
        Path     = $file
        Severity = @('Warning', 'Error')
    }
    if (Test-Path $settingsFile) {
        $analyzerArgs['Settings'] = $settingsFile
    }
    $issues = Invoke-ScriptAnalyzer @analyzerArgs
    if ($issues) {
        $hadFindings = $true
        Write-Host ""
        Write-Host "$file :" -ForegroundColor Red
        $issues | Format-Table RuleName, Severity, Line, Message -AutoSize | Out-String | Write-Host
    }
}

if ($hadFindings) {
    exit 1
}
