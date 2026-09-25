<#
    verify-clean.ps1 - read-only answer to "is this machine back to stock?"

    Performs exactly the checks tools\uninstall.ps1 performs, because both dot-source the same
    definitions from tools\deck-state.ps1. Mutates nothing, needs no elevation, and never claims
    success for a check it could not actually perform.

    EXIT CODES
      0 clean            this project left nothing behind
      1 leftovers        something from this project is still installed
      2 undetermined     at least one check could not be performed (and nothing failed outright)
#>
[CmdletBinding()]
param([switch] $Quiet)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')

if (-not $Quiet) {
    Write-Host ''
    Write-Host 'Verify clean state (read-only; changes nothing):' -ForegroundColor White
    Write-Host "  Elevated: $(Test-DeckElevated)   (some checks read more when elevated)"
    Write-Host ''
}

$evidence = @(
    @{ Path = $DeckDriversBefore; Note = 'stock package set' }
    @{ Path = $DeckBtBefore;      Note = 'pre-project radio + pairings' }
    @{ Path = $DeckCertFile;      Note = 'project test certificate' }
    @{ Path = $DeckBcdBackup;     Note = 'pre-project BCD store' }
    @{ Path = $DeckBaselineJson;  Note = 'captured baseline' }
)
foreach ($e in $evidence) {
    $state = if (Test-Path -LiteralPath $e.Path) { 'present' } else { 'MISSING' }
    if (-not $Quiet) { Write-Host ("  {0,-8} {1,-52} {2}" -f $state, (Split-Path -Leaf $e.Path), $e.Note) }
}

$checks = Get-DeckChecks
$leftovers = 0
$undetermined = 0

Write-Host ''
foreach ($c in $checks) {
    if ($c.Pass -eq $true) {
        Write-Host ("[CLEAN]     {0}" -f $c.Name) -ForegroundColor Green
        Write-Host ("            {0}   (source: {1})" -f $c.Detail, $c.Source) -ForegroundColor DarkGray
    } elseif ($null -eq $c.Pass) {
        $undetermined++
        Write-Host ("[UNKNOWN]   {0}" -f $c.Name) -ForegroundColor Yellow
        Write-Host ("            {0}   (source: {1})" -f $c.Detail, $c.Source) -ForegroundColor DarkGray
    } else {
        $leftovers++
        Write-Host ("[LEFTOVER]  {0}" -f $c.Name) -ForegroundColor Red
        Write-Host ("            {0}   (source: {1})" -f $c.Detail, $c.Source) -ForegroundColor DarkGray
    }
}

Write-Host ''
if ($leftovers -gt 0) {
    Write-Host "VERDICT: NOT STOCK - $leftovers check(s) report leftovers from this project." -ForegroundColor Red
    Write-Host '         Run tools\session.ps1 -Uninstall to remove them.' -ForegroundColor Red
    exit 1
}
if ($undetermined -gt 0) {
    Write-Host "VERDICT: UNDETERMINED - no leftovers found, but $undetermined check(s) could not be performed." -ForegroundColor Yellow
    Write-Host '         Re-run elevated for a complete answer.' -ForegroundColor Yellow
    exit 2
}
Write-Host 'VERDICT: STOCK - nothing from this project remains on this machine.' -ForegroundColor Green
exit 0
