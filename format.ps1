# format.ps1 — on-demand formatter/linter entry point.
#
# By default runs every pre-commit hook against every tracked file in the
# repo, rewriting formatting where the hooks support --fix. Use -Staged to
# limit the run to what's currently `git add`ed (useful mid-edit when you
# don't want to touch the whole tree).
#
# The actual hook set lives in .pre-commit-config.yaml. Tool install steps
# live in the README's "Linting and autoformatting" section.
#
# Exit code: 0 when the tree ends up clean, 1 only when something needs a
# human. `pre-commit` itself doesn't draw that line — it exits non-zero both
# when a hook rewrote a file for you (clang-format reformatting a .cpp,
# markdownlint --fix, prettier, the whitespace hooks) and when a hook found
# something it can't fix (a PSScriptAnalyzer finding, markdownlint's MD040,
# a syntax error). Those are very different outcomes and treating the first
# as failure trains you to ignore the exit code.
#
# So on a non-zero pass we re-run the same invocation. Fixes are already on
# disk by then, so a clean second pass proves every finding was auto-fixed
# and we exit 0; a second failure is the real thing and exits 1. The extra
# pass costs nothing on an already-clean tree, since it only runs on failure.
#
# The run also warns about untracked files. `pre-commit run --all-files`
# enumerates the git index, not the working tree, so a file you've created
# but not yet `git add`ed is skipped silently — the run reports every hook
# Passed without ever having opened it. That reads as coverage you don't
# have. The warning is advisory and doesn't affect the exit code.

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

$autofixed = @()
$unfixable = @()

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
    if ($LASTEXITCODE -eq 0) { continue }

    # Non-zero. Could be "a hook rewrote files for you" or "a hook found
    # something it can't fix" — pre-commit uses the same exit code for both.
    # The fixes are on disk now, so re-running settles which it was.
    $label = if ($hook) { $hook } else { 'hook set' }
    Write-Host ''
    Write-Host "==> $label reported findings; re-running to see whether its own fixes settled them" -ForegroundColor Cyan
    & $preCommitCmd[0] @argv

    if ($LASTEXITCODE -eq 0) {
        $autofixed += $label
    }
    else {
        $unfixable += $label
    }
}

# Untracked files the hooks never saw. `--exclude-standard` honours
# .gitignore, so build output and generated files don't show up here.
$untracked = @()
if (Get-Command git -ErrorAction SilentlyContinue) {
    $candidates = @(& git ls-files --others --exclude-standard 2>$null)
    if ($LASTEXITCODE -eq 0 -and $candidates) {
        # Mirrors the top-level `exclude:` in .pre-commit-config.yaml — keep
        # the two in sync. Those paths are outside the hooks' remit by
        # design, so an untracked file under one isn't being "missed".
        $outOfScope = @(
            '^esp/NarrativeEngine\.esp$',
            '^esp/plugin/',
            '^build/',
            '^dashboard/node_modules/',
            '^external/',
            '^docs/prior-art/'
        )
        $untracked = @($candidates | Where-Object {
                $path = $_
                -not ($outOfScope | Where-Object { $path -match $_ })
            })
    }
}

if ($untracked) {
    Write-Host ''
    Write-Host "==> $($untracked.Count) untracked file(s) were NOT checked — pre-commit only looks at files git tracks:" -ForegroundColor Yellow
    $untracked | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
    if ($untracked.Count -gt 20) {
        Write-Host "    ... and $($untracked.Count - 20) more" -ForegroundColor Yellow
    }
    Write-Host '    `git add -N <file>` is enough to pull them into the next run.' -ForegroundColor Yellow
}

Write-Host ''
if ($autofixed) {
    Write-Host "==> auto-fixed, tree is now clean: $($autofixed -join ', ')" -ForegroundColor Green
}
if ($unfixable) {
    Write-Host "==> findings remain after re-running: $($unfixable -join ', ')" -ForegroundColor Red
    Write-Host '    Scroll up to the second pass — whatever is still Failed there needs fixing by hand.' -ForegroundColor Red
    exit 1
}
if (-not $autofixed) {
    Write-Host '==> clean' -ForegroundColor Green
}
exit 0
