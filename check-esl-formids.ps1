# check-esl-formids.ps1 - validate that every new record in the Spriggit-serialized
# plugin tree under esp/plugin/ has a FormID inside the ESL-legal range.
#
# NarrativeEngine.esp is ESL-flagged, which means every record it *owns* (new records,
# not overrides of records that live in master plugins) must have a local FormID in
# [0x000800..0x000FFF]. The Creation Kit is supposed to enforce this when the light-plugin
# flag is set, but has been observed to occasionally assign out-of-range FormIDs anyway,
# which produces an ESP that appears fine in CK but silently breaks at load time.
#
# Spriggit writes one YAML file per record under esp/plugin/<RecordType>/, with the
# owning master and the record's local FormID baked into the filename:
#
#   <EditorID> - <6-hex-FormID>_<OwnerMaster>.yaml
#
# So detection is a filename-scan. Files whose owner is *not* NarrativeEngine.esp are
# overrides and skipped. Files whose FormID falls outside [0x800..0xFFF] are reported
# and the script exits non-zero.
#
# Wired into:
#   - the pre-commit hook installed by setup-mod-folder.ps1 (fails the commit)
#   - package.ps1 (fails the release-artifact build before zipping)
#
# Runs standalone for spot-checks. Zero external-tool dependency.

[CmdletBinding()]
param(
    [string]$PluginRoot = (Join-Path $PSScriptRoot 'esp/plugin'),
    [string]$OwnerMaster = 'NarrativeEngine.esp'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $PluginRoot -PathType Container)) {
    Write-Host "ESL FormID check: $PluginRoot does not exist; nothing to validate." -ForegroundColor DarkGray
    exit 0
}

# ESL legal range for new records in a light plugin: 0x000800..0x000FFF.
# Anything above 0xFFF has bits set in the middle of the local FormID that only make
# sense for full plugins; anything below 0x800 collides with reserved / injected slots.
$eslMin = 0x800
$eslMax = 0xFFF

# Filename shape: "<EditorID> - <hex6>_<master>.yaml". EditorIDs are alphanumeric +
# underscore per CK's own rules, so the pattern is unambiguous. Non-record files
# (spriggit-meta.json, RecordData.yaml, .gitkeep) simply don't match and are skipped.
$pattern = '^(?<edid>[A-Za-z0-9_]+) - (?<fid>[0-9A-Fa-f]{6})_(?<master>.+?)\.yaml$'

$violations = @()
$checked = 0

Get-ChildItem -LiteralPath $PluginRoot -Recurse -File -Filter '*.yaml' | ForEach-Object {
    $m = [regex]::Match($_.Name, $pattern)
    if (-not $m.Success) { return }

    # Overrides of records owned by other masters are constrained by the master's
    # FormID scheme, not ours - skip them.
    $master = $m.Groups['master'].Value
    if ($master -ne $OwnerMaster) { return }

    $checked++
    $editorId = $m.Groups['edid'].Value
    $formIdHex = $m.Groups['fid'].Value
    $formId = [Convert]::ToInt32($formIdHex, 16)

    if ($formId -lt $eslMin -or $formId -gt $eslMax) {
        $violations += [PSCustomObject]@{
            File     = $_.FullName
            EditorId = $editorId
            FormId   = "0x$($formIdHex.ToUpper())"
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "ESL FormID validation FAILED - $($violations.Count) out-of-range record(s):" -ForegroundColor Red
    $repoRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
    foreach ($v in $violations) {
        $rel = $v.File
        if ($rel.StartsWith($repoRoot)) {
            $rel = $rel.Substring($repoRoot.Length).TrimStart('\', '/')
        }
        Write-Host ("  {0,-40} {1,-10}  ({2})" -f $v.EditorId, $v.FormId, $rel) -ForegroundColor Red
    }
    Write-Host ''
    Write-Host "ESL-flagged plugins require new-record FormIDs in [0x000800..0x000FFF]." -ForegroundColor Yellow
    Write-Host 'Fix in the Creation Kit (compact / renumber the offending forms), re-save,' -ForegroundColor Yellow
    Write-Host 'then re-run sync-esp.ps1 to refresh the YAML tree before retrying.' -ForegroundColor Yellow
    exit 1
}

Write-Host "ESL FormID check passed: $checked record(s) owned by $OwnerMaster, all in [0x800..0xFFF]." -ForegroundColor Green
exit 0
