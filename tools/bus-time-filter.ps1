<#
    bus-time-filter.ps1 - Manage DeckBtFlt under BTHUSB on the emulated radio.

    On the tested build (Windows 11 25H2, build 26200) BTHUSB carries SCO voice without this filter.
    The filter's established use is the WinUSB isochronous test stack, which fails without a frame
    clock because UdeCx answers QueryBusTime with STATUS_NOT_SUPPORTED (see the isochronous
    reference documentation). Under BTHUSB it is a diagnostic for other Windows builds.

    If a voice run shows BTHUSB selecting a SCO alternate setting (ScoAltSetting > 0) with no SCO
    URBs (ScoOutUrbs = ScoInUrbs = 0), QueryBusTime is one hypothesis, not an established cause.
    Install the filter to test it, then run -Status: with IsochHookInstalled = 1, nonzero
    QueryBusTimeCalls or QueryBusTimeExCalls means BTHUSB requested bus time; zero rules it out.

      -Install  stage deckbtflt.inf (it matches the hardware ID USB\VID_0CF3&PID_6390 and so outranks
                bth.inf's compatible-ID match; BTHUSB/BTHPORT still drive the radio) and set
                IsochClockMode=2: call the lower stack first, synthesize a frame clock only when it
                fails. Then run tools\session.ps1 -Bridge -HoldSeconds 600.
      -Remove   delete every staged deckbtflt.inf package (run after the session).
      -Status   show staged packages and the filter's QueryBusTime counters from the last run.
#>
param([switch] $Install, [switch] $Remove, [switch] $Status)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')
if (-not (Test-DeckElevated)) { Write-Host 'REFUSAL: run elevated.'; exit 2 }
$root = Split-Path -Parent $PSScriptRoot
$inf = Join-Path $root 'src\filter\x64\Release\deckbtflt\deckbtflt.inf'
$paramsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtFlt\Parameters'

function Get-FilterPackages { @(Get-DeckStagedPackages | Where-Object { $_.IsOurs -and $_.OriginalName -ieq 'deckbtflt.inf' }) }

if ($Install) {
    if (-not (Test-Path -LiteralPath $inf)) { throw "Filter package not built: $inf (run tools\build.cmd)." }
    & pnputil.exe /add-driver $inf | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "pnputil /add-driver exit $LASTEXITCODE." }
    New-Item -Path $paramsKey -Force | Out-Null
    Set-ItemProperty -LiteralPath $paramsKey -Name IsochClockMode -Value 2 -Type DWord
    Write-Host ("FILTER STAGED: {0}; IsochClockMode=2. Now run tools\session.ps1 -Bridge." -f ((Get-FilterPackages | ForEach-Object { $_.Published }) -join ', '))
}
if ($Remove) {
    foreach ($p in Get-FilterPackages) {
        & pnputil.exe /delete-driver $p.Published /uninstall /force | Out-Host
    }
    Write-Host ("FILTER PACKAGES REMAINING: {0}" -f @(Get-FilterPackages).Count)
}
if ($Status -or (-not $Install -and -not $Remove)) {
    Write-Host ("staged: {0}" -f ((Get-FilterPackages | ForEach-Object { $_.Published }) -join ', '))
    $v = Get-ItemProperty -LiteralPath $paramsKey -ErrorAction SilentlyContinue
    if ($v) {
        foreach ($n in 'IsochClockMode', 'IsochClockModeActive', 'IsochHookInstalled', 'IsochHookSynthesizing', 'UsbdiQueryCount',
                       'QueryBusTimeCalls', 'QueryBusTimeExCalls', 'QueryBusTimeLastStatus', 'QueryBusTimeUnderlyingStatus') {
            if ($null -ne $v.$n) { Write-Host ('  {0,-30} {1}' -f $n, $v.$n) }
        }
    }
}
