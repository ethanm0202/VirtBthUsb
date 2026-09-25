<#
    sco-status.ps1 - SCO voice path telemetry from a running DeckBtUsb.

    Prints the USB radio's state, the SCO voice path (the same decoder uart-probe uses: Show-ScoPath),
    the opt-in loopback self-test results if that ran, and a SCO VOICE EVIDENCE line in uart-probe's
    format. Read-only; no elevation needed. Run it after python tools\miccheck.py.
#>
$ErrorActionPreference = 'Stop'
$paramsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'uart-probe.ps1'), [ref]$null, [ref]$null)
$fn = $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Show-ScoPath' }, $true) |
    Select-Object -First 1
. ([scriptblock]::Create($fn.Extent.Text))

$usb = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
Write-Host ('USB RADIO: {0}' -f $(if ($usb.Count) { '{0} {1}' -f $usb[0].InstanceId, $usb[0].Status } else { 'absent (DeckBtUsb is not serving)' }))
$v = Get-ItemProperty -LiteralPath $paramsKey -ErrorAction SilentlyContinue
if ($null -eq $v) { Write-Host 'No DeckBtUsb record.'; exit 1 }
Show-ScoPath $v
if ($v.UartScoLoopRan -eq 1) {
    Write-Host ('SCO LOOPBACK SELF-TEST: enter=0x{0:X3} links={1} scoHandle=0x{2:X4} sent={3} echoed={4} matched={5} leave=0x{6:X3} reset=0x{7:X3}' -f
        $v.UartScoLoopEnterStatus, $v.UartScoLoopConnections, $v.UartScoLoopScoHandle, $v.UartScoLoopSent,
        $v.UartScoLoopEchoed, $v.UartScoLoopMatched, $v.UartScoLoopLeaveStatus, $v.UartScoLoopResetStatus)
}
$field = { param($n) if ($null -ne $v.$n) { $v.$n } else { 0 } }
Write-Host ("SCO VOICE EVIDENCE: syncLinks={0} alt={1} usbOut={2}B/{3}pkts toController={4} fromController={5} usbIn={6}B framed={7} lost={8}" -f
    (& $field 'ScoLinkEvents'), (& $field 'ScoAltSetting'), (& $field 'ScoOutBytes'), (& $field 'ScoOutHciPackets'),
    (& $field 'UartBridgeScoOut'), (& $field 'UartBridgeScoIn'), (& $field 'ScoInBytes'),
    (& $field 'ScoInHciPackets'), (& $field 'UartBridgeScoLost'))
