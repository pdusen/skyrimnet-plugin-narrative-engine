# format.ps1 — on-demand formatter/linter entry point.
#
# By default runs every pre-commit hook against every tracked file in the
# repo, rewriting formatting where the hooks support --fix. Use -Staged to
# limit the run to what's currently `git add`ed (useful mid-edit when you
# don't want to touch the whole tree).
#
# The actual hook set lives in .pre-commit-config.yaml. Tool install steps
# live in the README's "Linting and autoformatting" section.

[CmdletBinding()]
param(
    # Run only against files currently staged in the index.
    [switch]$Staged,

    # Comma-separated hook IDs to run instead of the full set. Example:
    #   pwsh -File format.ps1 -Hooks clang-format,markdownlint
    [string]$Hooks
)

$ErrorActionPreference = 'Stop'

# Resolve how to invoke pre-commit. Order of preference:
#   1. `pre-commit` on PATH — fastest.
#   2. `python -m pre_commit` — works when pip's user-scripts directory
#      hasn't been added to PATH yet (common right after `pip install --user`
#      when the shell hasn't been restarted). No log-out required.
$preCommitCmd = $null
if (Get-Command pre-commit -ErrorAction SilentlyContinue) {
    $preCommitCmd = @('pre-commit')
}
else {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
    if ($py) {
        & $py.Source -c "import pre_commit" 2>$null
        if ($LASTEXITCODE -eq 0) {
            $preCommitCmd = @($py.Source, '-m', 'pre_commit')
        }
    }
}

if (-not $preCommitCmd) {
    Write-Error "pre-commit is not installed (or Python can't import pre_commit). See README 'Linting and autoformatting' for setup."
    exit 1
}

# `pre-commit run` accepts at most one hook ID per invocation, so loop when
# the caller passed a comma-separated list.
[string[]]$hookList = @('')
if ($Hooks) {
    $hookList = @(
        $Hooks.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }
    )
}

$failed = $false
foreach ($hook in $hookList) {
    # Build the argv as one flat array. Using [string[]] and appending with
    # += avoids the PowerShell trap where `if (...) { @('--all-files') }`
    # unwraps to a scalar string that then splats as a char array.
    [string[]]$argv = @()
    if ($preCommitCmd.Length -gt 1) {
        $argv += $preCommitCmd[1..($preCommitCmd.Length - 1)]
    }
    $argv += 'run'
    if ($hook) { $argv += $hook }
    if (-not $Staged) { $argv += '--all-files' }

    & $preCommitCmd[0] @argv

    if ($LASTEXITCODE -ne 0) {
        $failed = $true
    }
}

if ($failed) { exit 1 } else { exit 0 }
