@{
    # Rules deliberately silenced for this repo.
    ExcludeRules = @(
        # Write-Host is the right call for our user-facing setup/build scripts —
        # PSScriptAnalyzer's stock recommendation to prefer Write-Output is aimed
        # at library/pipeline scripts, not interactive tooling.
        'PSAvoidUsingWriteHost',

        # Our .ps1 files contain em-dashes / other Unicode in comments; there's
        # no reason to introduce a BOM just to appease this rule.
        'PSUseBOMForUnicodeEncodedFile',

        # False-positive prone: fires whenever a script-level param is only
        # referenced from a nested function (e.g. sync-esp.ps1's $DryRun,
        # consumed by Write-NoOp / Copy-Esp defined in the same file). The
        # analyzer only inspects the top-level scope.
        'PSReviewUnusedParameter'
    )
}
