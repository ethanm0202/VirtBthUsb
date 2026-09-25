<#
    bus-time-filter.ps1 - Manage DeckBtFlt under BTHUSB on the emulated radio.

    Use only if a voice run shows BTHUSB selecting a SCO alternate setting (ScoAltSetting > 0) but
    never streaming (ScoOutUrbs = ScoInUrbs = 0): the likely cause is BTHUSB asking for bus time,
    which native UdeCx refuses (WinUSB requires the same clock synthesis; see the isochronous
    reference documentation).

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
