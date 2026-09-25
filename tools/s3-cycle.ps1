<#
    s3-cycle.ps1 - Automated S3 sleep and resume cycles against the radio.

    Each cycle arms a resume-capable waitable timer, suspends to S3 (the hardware platform uses
    ACPI S3 sleep without modern standby), and after resume measures:
      - whether DeckBtUsb re-armed (ResumeRearms / ResumeLastStatus under the service Parameters),
      - how long until the USB radio (USB\VID_0CF3&PID_6390) and both Microsoft Bluetooth
        enumerators (BTH\MS_BTHBRB, BTH\MS_BTHLE) are OK again,
      - whether the paired-device count is unchanged.
    With -Baseline it measures the stock radio instead (QCA_SHB\UART_H4 child OK), which is how to
    learn whether this machine wakes from an RTC timer at all before involving the project driver.

    The AC "Allow wake timers" policy is switched to Enable for the run and restored afterwards.
    Requires elevation. Prints one CYCLE line per cycle and a final S3 CYCLES verdict.
#>
param(
    [ValidateRange(1, 1000)][int] $Cycles = 3,
    [ValidateRange(15, 3600)][int] $SleepSeconds = 30,
    [ValidateRange(10, 600)][int] $SettleSeconds = 90,
    [switch] $Baseline,
    [string] $LogPath
)
$ErrorActionPreference = 'Stop'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 's3-cycle.ps1 must run elevated.'
}
if ($LogPath) { Start-Transcript -LiteralPath $LogPath -Append | Out-Null }

Add-Type -Namespace DeckBt -Name Power -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern IntPtr CreateWaitableTimer(IntPtr attributes, bool manualReset, string name);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetWaitableTimer(IntPtr timer, ref long dueTime, int period, IntPtr completion, IntPtr arg, bool resume);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(IntPtr handle);
[DllImport("powrprof.dll", SetLastError = true)]
public static extern bool SetSuspendState(bool hibernate, bool force, bool wakeupEventsDisabled);
'@

$paramsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'
$rtcWake = 'bd3b718a-0680-4d9d-8ab2-e1d2b4ac806d'
$sleepGroup = '238c9fa8-0aad-41ed-83f4-97be242c8f20'

function Get-AcWakePolicy {
    $q = powercfg /q SCHEME_CURRENT $sleepGroup $rtcWake | Out-String
    if ($q -match 'Current AC Power Setting Index:\s*0x([0-9a-fA-F]+)') { return [Convert]::ToInt32($Matches[1], 16) }
    throw 'Cannot read the AC wake-timer policy.'
}

function Set-AcWakePolicy([int] $Index) {
    powercfg /setacvalueindex SCHEME_CURRENT $sleepGroup $rtcWake $Index | Out-Null
    powercfg /setactive SCHEME_CURRENT | Out-Null
}

function Get-RadioState {
    $present = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)
    $usb = @($present | Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
    $brb = @($present | Where-Object { $_.InstanceId -like 'BTH\MS_BTHBRB*' })
    $le = @($present | Where-Object { $_.InstanceId -like 'BTH\MS_BTHLE*' })
    $stock = @($present | Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4*' })
    $values = Get-ItemProperty -LiteralPath $paramsKey -ErrorAction SilentlyContinue
    [pscustomobject]@{
        UsbOk        = ($usb.Count -eq 1 -and $usb[0].Status -eq 'OK')
        UsbId        = $(if ($usb.Count) { $usb[0].InstanceId } else { '' })
        BrbOk        = ($brb.Count -ge 1 -and @($brb | Where-Object Status -ne 'OK').Count -eq 0)
        LeOk         = ($le.Count -ge 1 -and @($le | Where-Object Status -ne 'OK').Count -eq 0)
        StockOk      = ($stock.Count -eq 1 -and $stock[0].Status -eq 'OK')
        Rearms       = $(if ($values -and $null -ne $values.ResumeRearms) { [int]$values.ResumeRearms } else { 0 })
        RearmStatus  = $(if ($values -and $null -ne $values.ResumeLastStatus) { '0x{0:X8}' -f [uint32]$values.ResumeLastStatus } else { 'n/a' })
        Steady       = $(if ($values -and $null -ne $values.UartSteadyReached) { [int]$values.UartSteadyReached } else { 0 })
        PlugStatus   = $(if ($values -and $null -ne $values.UartUsbPlugStatus) { '0x{0:X8}' -f [uint32]$values.UartUsbPlugStatus } else { 'n/a' })
        HandbackBaud = $(if ($values -and $null -ne $values.UartHandbackBaud) { [int]$values.UartHandbackBaud } else { 0 })
        Replacements = $(if ($values -and $null -ne $values.ResumeReplacements) { [int]$values.ResumeReplacements } else { 0 })
        ReadyMs      = $(if ($values -and $null -ne $values.ResumeReadyMs) { [int]$values.ResumeReadyMs } else { -1 })
        PlugAttempts = $(if ($values -and $null -ne $values.ResumePlugAttempts) { [int]$values.ResumePlugAttempts } else { 0 })
        Commands     = $(if ($values -and $null -ne $values.UartBridgeCommands) { [int]$values.UartBridgeCommands } else { 0 })
        Paired       = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                         Where-Object { $_.InstanceId -like 'BTHENUM\DEV_*' -or $_.InstanceId -like 'BTHLE\DEV_*' }).Count
    }
}

# DeckBtUsb is up again only once the re-armed session has plugged in its replacement child: the
# old child keeps reporting OK until then, so the stack state alone cannot distinguish a resumed
# radio from a stale one.
function Test-RadioUp([object] $State, [object] $Before) {
    if ($Baseline) { return $State.StockOk }
    return ($State.Replacements -gt $Before.Replacements -and $State.Steady -eq 1 -and
            $State.PlugStatus -eq '0x00000000' -and $State.UsbOk -and $State.BrbOk -and $State.LeOk)
}

$before = Get-RadioState
$stackUp = if ($Baseline) { $before.StockOk } else { $before.UsbOk -and $before.BrbOk -and $before.LeOk -and $before.Steady -eq 1 }
if (-not $stackUp) {
    throw ('Radio is not up before cycling ({0}); start a bridge session first (tools\session.ps1 -Bridge) or use -Baseline.' -f
        ($before | ConvertTo-Json -Compress))
}
Write-Host ("[*] start: {0}" -f ($before | ConvertTo-Json -Compress))

$originalPolicy = Get-AcWakePolicy
$pass = 0
$fail = 0
$timer = [DeckBt.Power]::CreateWaitableTimer([IntPtr]::Zero, $true, $null)
if ($timer -eq [IntPtr]::Zero) { throw 'CreateWaitableTimer failed.' }
try {
    if ($originalPolicy -ne 1) { Set-AcWakePolicy 1 }
    for ($n = 1; $n -le $Cycles; $n++) {
        $pre = Get-RadioState
        $due = -1L * $SleepSeconds * 10000000L
        if (-not [DeckBt.Power]::SetWaitableTimer($timer, [ref]$due, 0, [IntPtr]::Zero, [IntPtr]::Zero, $true)) {
            throw ('SetWaitableTimer failed: {0}' -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        $t0 = Get-Date
        $suspended = [DeckBt.Power]::SetSuspendState($false, $false, $false)
        $t1 = Get-Date
        $slept = [int]($t1 - $t0).TotalSeconds
        if (-not $suspended -or $slept -lt [Math]::Min(10, $SleepSeconds - 5)) {
            Write-Host ("CYCLE {0}: FAIL: system did not suspend (SetSuspendState={1}, returned after {2}s)" -f $n, $suspended, $slept)
            $fail++
            continue
        }
        $upAfter = $null
        $state = $null
        $deadline = (Get-Date).AddSeconds($SettleSeconds)
        while ((Get-Date) -lt $deadline) {
            $state = Get-RadioState
            $rearmed = $Baseline -or $state.Rearms -gt $pre.Rearms
            if ($rearmed -and (Test-RadioUp $state $pre)) { $upAfter = [int]((Get-Date) - $t1).TotalSeconds; break }
            Start-Sleep -Seconds 1
        }
        # Paired devices re-enumerate after BTHPORT finishes initializing, which can take several seconds
        # after the radio reappears: judge only once all previously paired devices return or the settle
        # window expires.
        while ($null -ne $upAfter -and $state.Paired -lt $pre.Paired -and (Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 1
            $state = Get-RadioState
        }
        if ($null -eq $state) { $state = Get-RadioState }
        $ok = ($null -ne $upAfter) -and ($state.Paired -ge $pre.Paired)
        $detail = "slept {0}s; rearms {1}->{2} status={3}; radio up after {4} (driver ready {5} ms, plug attempts {6}); usb={7} brb={8} le={9} steady={10} plug={11} commands={12}; paired {13}->{14}" -f
            $slept, $pre.Rearms, $state.Rearms, $state.RearmStatus, $(if ($null -ne $upAfter) { "${upAfter}s" } else { "NOT within ${SettleSeconds}s" }),
            $state.ReadyMs, $state.PlugAttempts, $state.UsbOk, $state.BrbOk, $state.LeOk, $state.Steady, $state.PlugStatus,
            $state.Commands, $pre.Paired, $state.Paired
        if ($ok) { $pass++; Write-Host ("CYCLE {0}: PASS: {1}" -f $n, $detail) }
        else { $fail++; Write-Host ("CYCLE {0}: FAIL: {1}" -f $n, $detail) }
        if (-not $ok) { break }
        Start-Sleep -Seconds 5
    }
} finally {
    [void][DeckBt.Power]::CloseHandle($timer)
    if ((Get-AcWakePolicy) -ne $originalPolicy) { Set-AcWakePolicy $originalPolicy }
    Write-Host ("[*] AC wake-timer policy restored to index {0}" -f (Get-AcWakePolicy))
}
Write-Host ("S3 CYCLES: pass={0} fail={1} of {2} ({3})" -f $pass, $fail, $Cycles, $(if ($Baseline) { 'stock radio baseline' } else { 'DeckBtUsb' }))
if ($LogPath) { Stop-Transcript | Out-Null }
exit $(if ($fail -eq 0 -and $pass -eq $Cycles) { 0 } else { 1 })
