<#
    uart-probe.ps1 - firmware probe and bridge operator. Every device, package and registry
    operation runs in a disposable PowerShell process with a hard wall-clock deadline. Killing
    that process does not cancel a wedged kernel IRP: after a timeout no further state access
    is attempted.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $DryRun,
    [switch] $Force,
    [switch] $KeepBound,
    [switch] $Bridge,
    [ValidateRange(0, 900)][int] $HoldSeconds = 0,
    [ValidateRange(1, 600)][int] $PnpTimeoutSeconds = 60,
    [string] $LogPath
)
$ErrorActionPreference = 'Stop'
$toolsDir = if ($PSScriptRoot) { $PSScriptRoot } else { '.' }
$root = Split-Path -Parent ([IO.Path]::GetFullPath($toolsDir))
. (Join-Path $toolsDir 'deck-state.ps1')
$packageDir = Join-Path $root "src\driver\x64\$Configuration\deckbtusb"
$driverInfName = 'deckbtusb.inf'
$driverSysName = 'deckbtusb.sys'
$driverCatName = 'deckbtusb.cat'
$driverService = 'DeckBtUsb'
$driverInf = Join-Path $packageDir $driverInfName
$targetId = $RadioAcpiId
$paramsKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$driverService\Parameters"
$devGen = $DevGenPath
$vendorInfStore = Join-Path $VendorStoreDir 'qcbtuart.inf'
$vendorInfBackup = Join-Path $VendorBackupDir 'qcbtuart.inf'
$script:primaryExit = 0
$script:verdict = 'NOT RUN'
$script:wedged = $false
$script:mutated = $false
$script:packageAttempted = $false
$script:rootAttempted = $false
$script:stagedOemInf = $null
$script:transcriptStarted = $false
# Every bounded worker gets at least 10 s ([Math]::Max(10, ...)): a fresh PowerShell worker needs
# 1-2 s just to start, so a shorter budget misreports a slow start as "PnP appears wedged".

function Write-Step([string] $Message) { Write-Host "[*] $Message" -ForegroundColor Cyan }
function Write-Bad([string] $Message) { Write-Host "[-] $Message" -ForegroundColor Red }

function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments = @(), [int] $TimeoutSeconds = $PnpTimeoutSeconds) {
    if ($script:wedged) { throw 'PnP appears wedged; further device/registry operations are refused.' }
    $resultPath = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
    $payload = [Management.Automation.PSSerializer]::Serialize(@{
        Action = $Action.ToString(); Arguments = $Arguments; ResultPath = $resultPath
        StatePath = (Join-Path $PSScriptRoot 'deck-state.ps1')
    })
    $encodedPayload = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($payload))
    $command = @"
`$ErrorActionPreference = 'Stop'
`$p = [Management.Automation.PSSerializer]::Deserialize([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('$encodedPayload')))
try {
    . `$p.StatePath
    `$a = `$p.Arguments
    # Materialize the success stream: AutomationNull otherwise becomes a truthy empty PSObject in CLIXML.
    `$value = @(& ([scriptblock]::Create(`$p.Action)) @a)
    @{ Success = `$true; Value = `$value } | Export-Clixml -LiteralPath `$p.ResultPath
} catch {
    @{ Success = `$false; Error = `$_.Exception.Message } | Export-Clixml -LiteralPath `$p.ResultPath
}
"@
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $start.FileName)) { $start.FileName = Join-Path $PSHOME 'pwsh.exe' }
    $start.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        Write-Step "$Label (hard timeout ${TimeoutSeconds}s)"
        if (-not $process.Start()) { throw "Could not start worker: $Label" }
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $script:wedged = $true
            $script:primaryExit = 1
            $script:verdict = "FAIL: timed out: $Label"
            # Never WaitForExit after Kill: kernel work may remain stuck indefinitely.
            try { $process.Kill() } catch { }
            throw "PnP appears wedged: $Label exceeded ${TimeoutSeconds}s. Skipping directly to restore reporting."
        }
        if (-not (Test-Path -LiteralPath $resultPath)) { throw "Worker failed without a result: $Label (exit $($process.ExitCode))" }
        $result = Import-Clixml -LiteralPath $resultPath
        if (-not $result.Success) { throw "$Label failed: $($result.Error)" }
        return $result.Value
    } finally {
        $process.Dispose()
        Remove-Item -LiteralPath $resultPath -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-Pnp([string[]] $Arguments) {
    $r = Invoke-Bounded ('pnputil ' + ($Arguments -join ' ')) {
        param($NativeArgs)
        Invoke-DeckNative pnputil.exe $NativeArgs
    } @(,$Arguments)
    Write-Host $r.Output
    if ($r.Code -notin @(0, 3010)) { throw "pnputil failed (exit $($r.Code)): $($Arguments -join ' ')" }
    return $r
}

function Get-Target([string] $Id = $targetId, [int] $TimeoutSeconds = $PnpTimeoutSeconds) {
    Invoke-Bounded "Read device $Id" {
        param($Id)
        $pattern = if ($Id.EndsWith('*')) { $Id } else { "$Id*" }
        $nodes = @(Get-DeckCheckedDevices $pattern -PresentOnly)
        if ($nodes.Count -eq 0) {
            $null
        } elseif ($nodes.Count -gt 1) {
            throw "REFUSAL: device identity for '$Id' is ambiguous; expected at most one present device but found $($nodes.Count)."
        } else {
            $d = $nodes[0]
            $svc = Get-DeckNodeProperty $d.InstanceId DEVPKEY_Device_Service -Optional
            $problemStatus = Get-DeckNodeProperty $d.InstanceId DEVPKEY_Device_ProblemStatus -Optional
            $problemStatusText = if ($null -eq $problemStatus) { 'unavailable' } else { '0x{0:X8}' -f $problemStatus }
            [pscustomobject]@{ InstanceId = $d.InstanceId; Service = $svc; Status = $d.Status; Problem = $d.Problem; ProblemStatus = $problemStatusText }
        }
    } @($Id) $TimeoutSeconds
}

function Get-ProbeValues([int] $TimeoutSeconds = $PnpTimeoutSeconds) {
    Invoke-Bounded 'Read probe registry results' {
        param($Key)
        if (-not (Test-Path -LiteralPath $Key)) { return $null }
        $first = Get-ItemProperty -LiteralPath $Key -ErrorAction Stop
        $marker = $first.UartCompletion
        if ($null -ne $marker -and ($marker -eq 1 -or $marker -eq 2)) {
            return (Get-ItemProperty -LiteralPath $Key -ErrorAction Stop)
        }
        return $first
    } @($paramsKey) $TimeoutSeconds
}

function Get-ProbePackages {
    @(Invoke-Bounded 'Enumerate staged packages' { Get-DeckStagedPackages } |
        Where-Object { $_.IsOurs -and $_.OriginalName -ieq $driverInfName })
}

function Get-RootClaimingPackages {
    @(Invoke-Bounded 'Enumerate root-claiming packages' { Get-DeckStagedPackages } |
        Where-Object { $_.OriginalName -in @('deckbtusb.inf', 'isotest.inf') })
}

function Show-Ep0Trace([object] $Values) {
    Write-Host 'DECODED EP0 CONTROL TRACE:'
    if ($null -eq $Values -or $null -eq $Values.ControlCount -or $Values.ControlCount -le 0 -or $null -eq $Values.ControlLog) {
        Write-Host '  (no EP0 control transfers recorded)'
        return
    }
    $count = [int]$Values.ControlCount
    $logBytes = [byte[]]$Values.ControlLog
    if ($logBytes.Length -lt 12) {
        Write-Host "  (ControlLog buffer too small: $($logBytes.Length) bytes)"
        return
    }
    $totalInLog = [Math]::Min($count, 128)
    $maxSlots = [Math]::Min(128, [Math]::Floor($logBytes.Length / 12))
    $countToPrint = [Math]::Min($totalInLog, $maxSlots)
    $startIndex = $count - $countToPrint
    for ($i = $startIndex; $i -lt $count; $i++) {
        $slot = $i % 128
        $offset = $slot * 12
        if ($offset + 12 -le $logBytes.Length) {
            $outcomeCode = $logBytes[$offset + 8]
            $opcode = [BitConverter]::ToUInt16($logBytes, $offset + 10)
            $outcomeStr = switch ($outcomeCode) {
                1 { 'accepted' }
                2 { 'stalled' }
                3 { 'bad buffer' }
                4 { 'not class control' }
                default { "outcome=$outcomeCode" }
            }
            Write-Host ('  Transfer {0,4}: Opcode 0x{1:X4} ({2})' -f $i, $opcode, $outcomeStr)
        }
    }
}

# Decodes the driver's steady-state inbound event trace (EventLog: 128 slots x 8 bytes = event
# code, parameter length, first six parameter bytes; EventCount: total events captured).
function Show-EventTrace([object] $Values) {
    Write-Host 'DECODED HCI EVENT TRACE:'
    if ($null -eq $Values -or $null -eq $Values.EventCount -or $Values.EventCount -le 0 -or $null -eq $Values.EventLog) {
        Write-Host '  (no events recorded)'
        return
    }
    $count = [int]$Values.EventCount
    $log = [byte[]]$Values.EventLog
    $slots = [Math]::Floor($log.Length / 8)
    $first = [Math]::Max(0, $count - $slots)
    for ($i = $first; $i -lt $count; $i++) {
        $o = ($i % $slots) * 8
        $code = $log[$o]
        $p = $log[($o + 2)..($o + 7)]
        $hex = ($p | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
        $text = switch ($code) {
            0x03 { 'Connection Complete status=0x{0:X2} handle=0x{1:X4} addr={2}' -f $p[0], ([int]$p[1] + 256 * [int]$p[2]), (($p[3..5] | ForEach-Object { '{0:X2}' -f $_ }) -join ':') }
            0x05 { 'Disconnection Complete status=0x{0:X2} handle=0x{1:X4} reason=0x{2:X2}' -f $p[0], ([int]$p[1] + 256 * [int]$p[2]), $p[3] }
            0x08 { 'Encryption Change status=0x{0:X2} handle=0x{1:X4} enabled=0x{2:X2}' -f $p[0], ([int]$p[1] + 256 * [int]$p[2]), $p[3] }
            0x0E { 'Command Complete op=0x{0:X4} status=0x{1:X2}' -f ([int]$p[1] + 256 * [int]$p[2]), $p[3] }
            0x0F { 'Command Status   op=0x{0:X4} status=0x{1:X2}' -f ([int]$p[2] + 256 * [int]$p[3]), $p[0] }
            0x3E {
                switch ($p[0]) {
                    0x01 { 'LE Connection Complete status=0x{0:X2} handle=0x{1:X4} role=0x{2:X2}' -f $p[1], ([int]$p[2] + 256 * [int]$p[3]), $p[4] }
                    0x0A { 'LE Enhanced Connection Complete status=0x{0:X2} handle=0x{1:X4} role=0x{2:X2}' -f $p[1], ([int]$p[2] + 256 * [int]$p[3]), $p[4] }
                    0x05 { 'LE LTK Request     handle=0x{0:X4}' -f ([int]$p[1] + 256 * [int]$p[2]) }
                    default { 'LE Meta          subevent=0x{0:X2} params={1}' -f $p[0], $hex }
                }
            }
            default { 'Event 0x{0:X2} len={1} params={2}' -f $code, $log[$o + 1], $hex }
        }
        Write-Host ('  Event {0,4}: {1}' -f $i, $text)
    }
}

# Decodes the driver's advertiser table (AdvSeen: 48 slots x 12 bytes = address LSB first,
# address type, last event type, report count LE32; AdvSeenCount: slots used), busiest first.
function Show-AdvSeen([object] $Values) {
    Write-Host 'LE ADVERTISERS HEARD BY THE CONTROLLER:'
    if ($null -eq $Values -or $null -eq $Values.AdvSeenCount -or $Values.AdvSeenCount -le 0 -or $null -eq $Values.AdvSeen) {
        Write-Host '  (none recorded)'
        return
    }
    $t = [byte[]]$Values.AdvSeen
    $rows = for ($i = 0; $i -lt [Math]::Min([int]$Values.AdvSeenCount, [Math]::Floor($t.Length / 12)); $i++) {
        $o = $i * 12
        [pscustomobject]@{
            Address = (($t[($o + 5)..$o] | ForEach-Object { '{0:X2}' -f $_ }) -join ':')
            Type    = if ($t[$o + 6] -eq 0) { 'public' } else { 'random' }
            Event   = '0x{0:X2}' -f $t[$o + 7]
            Count   = [BitConverter]::ToUInt32($t, $o + 8)
        }
    }
    foreach ($r in ($rows | Sort-Object Count -Descending)) {
        Write-Host ('  {0}  {1,-6} lastEvent={2} reports={3}' -f $r.Address, $r.Type, $r.Event, $r.Count)
    }
}

# Decodes the SCO voice path: the frontend's Sco* counters (endpoints.c), the latest synchronous
# command BTHPORT sent (ScoSetupCommand: opcode LE16, length, parameters) and the controller's latest
# Synchronous Connection Complete/Changed (ScoLinkEvent: code, length, 17 parameters).
function Show-ScoPath([object] $Values) {
    Write-Host 'SCO VOICE PATH:'
    if ($null -eq $Values) {
        Write-Host '  (no record)'
        return
    }
    $names = @('ScoAltSetting', 'ScoAltChanges', 'ScoOutMaxPacket', 'ScoInMaxPacket', 'ScoOutUrbs', 'ScoInUrbs',
               'ScoOutIsoPackets', 'ScoInIsoPackets', 'ScoOutBytes', 'ScoInBytes', 'ScoOutHciPackets', 'ScoOutRejected',
               'ScoOutRingDrops', 'ScoOutResyncSkips', 'ScoInHciPackets', 'ScoInSourcePackets', 'ScoInSourceRejected',
               'ScoInDroppedBytes', 'ScoInLastSourceLength', 'ScoBadUrbs', 'ScoUnpacedUrbs', 'ScoFlushed',
               'ScoMaxLateUs', 'ScoSetupCommands', 'UartScoRouteRewritten', 'UartScoRouteRestored',
               'UartBridgeScoOut', 'UartBridgeScoIn', 'UartBridgeScoLost')
    $line = foreach ($n in $names) { if ($null -ne $Values.$n) { '{0}={1}' -f ($n -replace '^(Sco|UartBridge)', ''), $Values.$n } }
    Write-Host ('  ' + ($(if ($line) { $line -join ' ' } else { '(no SCO activity recorded)' })))
    foreach ($g in 'ScoFirstOutGeometry', 'ScoFirstInGeometry') {
        if ($null -ne $Values.$g -and $Values.$g -ne 0) {
            Write-Host ('  {0}: {1} packets, {2} bytes' -f $g, ([uint32]$Values.$g -shr 16), ([uint32]$Values.$g -band 0xFFFF))
        }
    }
    if ($null -ne $Values.ScoSetupCommands -and $Values.ScoSetupCommands -gt 0 -and $Values.ScoSetupCommand) {
        $c = [byte[]]$Values.ScoSetupCommand
        $op = [int]$c[0] + 256 * [int]$c[1]
        $name = switch ($op) { 0x0428 { 'Setup_Synchronous_Connection' } 0x0429 { 'Accept_Synchronous_Connection' }
                               0x043D { 'Enhanced_Setup_Synchronous_Connection' } 0x043E { 'Enhanced_Accept_Synchronous_Connection' }
                               0x0C26 { 'Write_Voice_Setting' } default { 'op' } }
        $len = [Math]::Min([int]$c[2], $c.Length - 3)
        Write-Host ('  last sync command: {0} (0x{1:X4}) params={2}' -f $name, $op, (($c[3..(2 + $len)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' '))
        # Byte index of Voice_Setting after the 3-byte command header (Core Vol 4 Part E 7.1.26/7.1.27, 7.3.28).
        $voice = switch ($op) { 0x0428 { 15 } 0x0429 { 19 } 0x0C26 { 3 } default { -1 } }
        if ($voice -ge 0 -and $c.Length -gt $voice + 1) {
            Write-Host ('  voice setting 0x{0:X4} (air coding {1})' -f ([int]$c[$voice] + 256 * [int]$c[$voice + 1]),
                        $(switch ($c[$voice] -band 3) { 0 { 'CVSD' } 1 { 'u-law' } 2 { 'A-law' } 3 { 'transparent (mSBC)' } }))
        }
    }
    if ($null -ne $Values.ScoLinkEvents -and $Values.ScoLinkEvents -gt 0 -and $Values.ScoLinkEvent) {
        $e = [byte[]]$Values.ScoLinkEvent
        $air = switch ($e[18]) { 0 { 'u-law' } 1 { 'A-law' } 2 { 'CVSD' } 3 { 'transparent' } default { '0x{0:X2}' -f $e[18] } }
        Write-Host ('  controller link event 0x{0:X2} (#{1}): status=0x{2:X2} handle=0x{3:X4} type={4} interval={5} rx={6} tx={7} air={8}' -f
            $e[0], $Values.ScoLinkEvents, $e[2], ([int]$e[3] + 256 * [int]$e[4]), $(if ($e[11] -eq 2) { 'eSCO' } else { 'SCO' }),
            $e[12], ([int]$e[14] + 256 * [int]$e[15]), ([int]$e[16] + 256 * [int]$e[17]), $air)
    }
}

function Show-Recovery {
    Write-Bad 'Automatic restore is not verified. Stop device and service operations; preserve this transcript.'
    if ($script:wedged) {
        Write-Host 'STOP: an operation timed out or driver retirement is unconfirmed. A responsive desktop does not prove UART release.'
        Write-Host 'Do not rerun this probe, uninstall the driver, or run full rollback in this boot.'
        Write-Host "Preserve this transcript. Staged package: $($script:stagedOemInf); service: $driverService."
        Write-Host 'Save work if possible. A restart may hang while PnP is wedged; if Windows stops responding, a hard power-off may be required.'
        Write-Host 'After a successful boot, use tools\session.ps1 -Stop once from an elevated console; if it fails, stop and preserve its transcript.'
        return
    }
    Write-Host 'After the machine is responsive again, run these commands from an elevated console:'
    Write-Host "  reg delete HKLM\SYSTEM\CurrentControlSet\Services\$driverService\Parameters /v UartProbe /f"
    if ($Bridge) {
        Write-Host "  reg delete HKLM\SYSTEM\CurrentControlSet\Services\$driverService\Parameters /v Enabled /f"
    }
    if ($script:stagedOemInf) {
        Write-Host "  pnputil /delete-driver $($script:stagedOemInf) /uninstall /force"
    } elseif ($script:packageAttempted) {
        Write-Host '  pnputil /enum-drivers'
        Write-Host "  Locate Published Name for Original Name $driverInfName, Provider DeckBtUsb; then:"
        Write-Host '  pnputil /delete-driver <that-published-oemN.inf> /uninstall /force'
    }
    # Reporting after a timeout must perform no further filesystem/device/registry queries.
    $vendor = $vendorInfBackup
    Write-Host "  pnputil /add-driver `"$vendor`" /install"
    Write-Host "  pnputil /enable-device /deviceid `"$RadioAcpiId`""
    Write-Host "  pnputil /restart-device /deviceid `"$RadioAcpiId`""
    Write-Host '  pnputil /scan-devices'
    Write-Host '  tools\session.ps1 -Uninstall'
    Write-Host 'A console timeout cannot cancel a kernel PnP hang. If the machine hangs, a hard power-off may be required.'
}

try {
    $title = if ($Bridge) { 'DeckBtUsb bridge operator' } else { 'DeckBtUsb probe operator' }
    Write-Host $title
    Write-Host ("Target: $targetId;" + ($(if ($Bridge) { " Bridge=True;" } else { "" })) + " DryRun=$([bool]$DryRun); PnP timeout=${PnpTimeoutSeconds}s")
    $probeWaitSeconds = if ($Bridge) { 30 } else { 75 }
    $releaseWaitSeconds = 30
    $startPlan = 'enable target only if still disabled after install; never restart an active probe'
    $cleanupPlan = 'uninstall; verify restore'
    if ($Bridge) {
        Write-Host "Plan (bridge): validate package; configure bridge (Enabled=1); install $driverInf; $startPlan; wait up to 90s for steady state; wait up to 60s for USB radio child; wait up to 30s for Microsoft enumerators; verify BTHPORT initialization; uninstall; wait up to 30s for release; verify restore."
    } else {
        Write-Host "Plan: validate package; configure probe; install $driverInf; $startPlan; wait up to ${probeWaitSeconds}s for recorded completion; report results; clear probe; $cleanupPlan."
    }
    if ($DryRun) {
        $script:verdict = 'DRY-RUN: no mutation (including no log files or registry access)'
    } else {
        if (-not (Test-DeckElevated)) { $script:primaryExit = 2; throw 'Administrator elevation required; this script does not elevate itself.' }
        $requiredFiles = @($driverSysName, $driverInfName, $driverCatName, 'hpbtfw21.tlv', 'hpnv21.bin')
        foreach ($name in $requiredFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $packageDir $name) -PathType Leaf)) { $script:primaryExit = 2; throw "Missing package file: $name" }
        }
        if (-not (Test-Path -LiteralPath $DeckBaselineJson)) { $script:primaryExit = 2; throw 'Baseline missing; capture baseline before taking the radio.' }
        if (-not ((Test-Path -LiteralPath $vendorInfStore) -or (Test-Path -LiteralPath $vendorInfBackup))) { $script:primaryExit = 2; throw 'Vendor recovery package missing.' }
        $node = Get-Target
        if (-not $node) { $script:primaryExit = 2; throw 'Radio devnode missing.' }
        $child = Get-Target $RadioChildLike
        if ($node.Status -eq 'OK' -and $child -and $child.Status -eq 'OK') { $script:primaryExit = 2; throw 'Vendor radio is active. Disable it first (tools\session.ps1 does this) before a probe.' }
        $existingPackages = @(Get-ProbePackages)
        if ($existingPackages.Count) {
            $script:primaryExit = 2
            throw ("REFUSAL: a matching project package is already staged: " + (($existingPackages | ForEach-Object { "$($_.Published) [$($_.OriginalName)]" }) -join ', '))
        }
        if (-not $Force) {
            $prompt = 'Physical mode claims the radio. Type PROBE to proceed'
            if ((Read-Host $prompt) -ne 'PROBE') { throw 'Operator refused; nothing changed.' }
        }
        if ([string]::IsNullOrWhiteSpace($LogPath)) {
            $prefix = if ($Bridge) { 'bridge' } else { 'probe' }
            $LogPath = Join-Path $root ("tools\_build\sessions\$prefix-{0}.log" -f (Get-Date -Format yyyyMMdd-HHmmss))
        }
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent ([IO.Path]::GetFullPath($LogPath))) -Force)
        Start-Transcript -Path $LogPath | Out-Null
        $script:transcriptStarted = $true
        $script:mutated = $true
        if ($Bridge) {
            Invoke-Bounded 'Clear stale results and arm Enabled' {
                param($Key)
                [void](New-Item -Path $Key -Force)
                $old = Get-ItemProperty -LiteralPath $Key
                foreach ($p in $old.PSObject.Properties) {
                    if ($p.Name -like 'Uart*' -or $p.Name -like 'Sco*') { Remove-ItemProperty -LiteralPath $Key -Name $p.Name }
                }
                Set-ItemProperty -LiteralPath $Key -Name Enabled -Value 1 -Type DWord
            } @($paramsKey)
        } else {
            Invoke-Bounded 'Clear stale results and arm UartProbe' {
                param($Key)
                [void](New-Item -Path $Key -Force)
                $old = Get-ItemProperty -LiteralPath $Key
                foreach ($p in $old.PSObject.Properties) {
                    if ($p.Name -like 'Uart*') { Remove-ItemProperty -LiteralPath $Key -Name $p.Name }
                }
                Set-ItemProperty -LiteralPath $Key -Name UartProbe -Value 1 -Type DWord
            } @($paramsKey)
        }
        $script:packageAttempted = $true
        # Stage first, so the exact published name is known before a PnP start can wedge.
        [void](Invoke-Pnp @('/add-driver', $driverInf))
        $packages = @(Get-ProbePackages)
        if ($packages.Count -ne 1) { throw 'Cannot uniquely identify staged probe package; refusing install.' }
        $script:stagedOemInf = $packages[0].Published
        [void](Invoke-Pnp @('/add-driver', $driverInf, '/install'))
        $node = Get-Target
        if (-not $node) { throw 'Target missing after install.' }
        # The token was armed before /install, which already starts any enabled device.
        if ("$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')) {
            [void](Invoke-Pnp @('/enable-device', $node.InstanceId))
        }
        # Root /install already starts the device; redundant enable is unsupported on Home.
        if ($Bridge) {
            # --- Bridge Session ---
            # 4. Wait up to 90s for steady state
            $bridgeDeadline = [DateTime]::UtcNow.AddSeconds(90)
            $steady = $false
            do {
                $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($bridgeDeadline - [DateTime]::UtcNow).TotalSeconds)))
                $node = Get-Target -TimeoutSeconds $remaining
                Assert-DeckNoDeviceProblem $node
                if ([DateTime]::UtcNow -ge $bridgeDeadline) { break }
                $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($bridgeDeadline - [DateTime]::UtcNow).TotalSeconds)))
                $values = Get-ProbeValues -TimeoutSeconds $remaining
                if ($null -ne $values) {
                    if ($values.UartCompletion -eq 2) {
                        $script:wedged = $true
                        $script:primaryExit = 1
                        $script:verdict = "FAIL: driver reported unconfirmed retirement (UartCompletion=2); UART work may remain active in kernel."
                        $values.PSObject.Properties | Where-Object {
                            $_.Name -like 'Uart*' -or $_.Name -in @('LastAddDeviceStep', 'LastAddDeviceStatus')
                        } | Sort-Object Name | ForEach-Object {
                            Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value)
                        }
                        throw $script:verdict
                    }
                    if ($values.UartCompletion -eq 1) {
                        $script:primaryExit = 1
                        $script:verdict = "FAIL: bridge bring-up ended unexpectedly with Completion=1 (phase=$($values.UartFailurePhase), step=$($values.UartLastStep), status=$($values.UartLastStatus))"
                        Write-Host 'UART BREADCRUMBS'
                        $values.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } | Sort-Object Name | ForEach-Object {
                            Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value)
                        }
                        throw $script:verdict
                    }
                    if ($values.UartFailurePhase -eq 'Steady' -and $values.UartSteadyReached -eq 1 -and $values.UartCompletion -eq 0) {
                        $steady = $true
                        break
                    }
                }
                Start-Sleep -Milliseconds 500
            } while ([DateTime]::UtcNow -lt $bridgeDeadline)

            if (-not $steady) {
                $script:primaryExit = 1
                $script:verdict = "FAIL: bridge did not reach Steady state within 90s."
                if ($values) {
                    $values.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } | Sort-Object Name | ForEach-Object {
                        Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value)
                    }
                }
                throw $script:verdict
            }

            # 5. Wait up to 60s for present USB radio matching USB\VID_0CF3&PID_6390* with Status 'OK'
            $usbDeadline = [DateTime]::UtcNow.AddSeconds(60)
            $usbDevice = $null
            do {
                $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($usbDeadline - [DateTime]::UtcNow).TotalSeconds)))
                $usbDevice = Get-Target 'USB\VID_0CF3&PID_6390*' -TimeoutSeconds $remaining
                if ($usbDevice) {
                    $hasProb = "$($usbDevice.Problem)" -notin @('', '0', 'CM_PROB_NONE')
                    if ($usbDevice.Status -eq 'Error' -or $hasProb) {
                        Write-Bad ("USB radio reported problem: Status={0}; Problem={1}; ProblemStatus={2}" -f $usbDevice.Status, $usbDevice.Problem, $usbDevice.ProblemStatus)
                        break
                    }
                    if ($usbDevice.Status -eq 'OK') {
                        break
                    }
                }
                if ([DateTime]::UtcNow -ge $usbDeadline) { break }
                Start-Sleep -Milliseconds 500
            } while ([DateTime]::UtcNow -lt $usbDeadline)

            $enumWaitSeconds = if ($enumWaitSeconds) { $enumWaitSeconds } else { 30 }
            $enumsHealthy = $false
            $enums = $null

            if ($usbDevice -and $usbDevice.Status -eq 'OK') {
                Write-Host ("USB RADIO APPEARED: InstanceId={0}; Status={1}; Problem={2}; Service={3}" -f `
                    $usbDevice.InstanceId, $usbDevice.Status, $usbDevice.Problem, $usbDevice.Service)

                # Poll up to 30s until BOTH Microsoft enumerators (BTH\MS_BTHBRB* and BTH\MS_BTHLE*) are present with Status OK
                $enumDeadline = [DateTime]::UtcNow.AddSeconds($enumWaitSeconds)
                do {
                    $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($enumDeadline - [DateTime]::UtcNow).TotalSeconds)))
                    # Check USB radio for early problem code
                    $usbDevice = Get-Target 'USB\VID_0CF3&PID_6390*' -TimeoutSeconds $remaining
                    if ($usbDevice) {
                        $hasProb = "$($usbDevice.Problem)" -notin @('', '0', 'CM_PROB_NONE')
                        if ($usbDevice.Status -eq 'Error' -or $hasProb) {
                            Write-Bad ("USB radio degraded during enumerator wait: Status={0}; Problem={1}; ProblemStatus={2}" -f `
                                $usbDevice.Status, $usbDevice.Problem, $usbDevice.ProblemStatus)
                            break
                        }
                    }
                    $enums = Invoke-Bounded 'Query Microsoft Bluetooth enumerators' {
                        $brbNodes = @(Get-PnpDevice -InstanceId 'BTH\MS_BTHBRB*' -PresentOnly -ErrorAction SilentlyContinue)
                        $leNodes = @(Get-PnpDevice -InstanceId 'BTH\MS_BTHLE*' -PresentOnly -ErrorAction SilentlyContinue)
                        $b = if ($brbNodes.Count) { $brbNodes[0] } else { $null }
                        $l = if ($leNodes.Count) { $leNodes[0] } else { $null }
                        [pscustomobject]@{
                            BrbPresent = ($null -ne $b); BrbStatus = if ($b) { $b.Status } else { $null }
                            LePresent  = ($null -ne $l); LeStatus  = if ($l) { $l.Status } else { $null }
                        }
                    } @() $remaining

                    if ($enums -and $enums.BrbPresent -and $enums.BrbStatus -eq 'OK' -and $enums.LePresent -and $enums.LeStatus -eq 'OK') {
                        $enumsHealthy = $true
                        break
                    }
                    if ([DateTime]::UtcNow -ge $enumDeadline) { break }
                    Start-Sleep -Milliseconds 500
                } while ([DateTime]::UtcNow -lt $enumDeadline)

                # Re-read USB radio at verdict time
                $remaining = [Math]::Min($PnpTimeoutSeconds, 10)
                $usbDevice = Get-Target 'USB\VID_0CF3&PID_6390*' -TimeoutSeconds $remaining

                if ($enumsHealthy) {
                    Write-Host 'MICROSOFT ENUMERATORS HEALTHY: BTH\MS_BTHBRB (BR/EDR) and BTH\MS_BTHLE (LE) are present and OK.'
                    # Wait ~3 s for steady-state counters to settle before dumps/verdict
                    Start-Sleep -Seconds 3
                } else {
                    $brbStatus = if ($enums -and $enums.BrbPresent) { $enums.BrbStatus } else { 'Absent' }
                    $leStatus = if ($enums -and $enums.LePresent) { $enums.LeStatus } else { 'Absent' }
                    Write-Bad ("MICROSOFT ENUMERATORS NOT HEALTHY within ${enumWaitSeconds}s: BTHBRB={0}; BTHLE={1}" -f $brbStatus, $leStatus)
                }
            }

            # Print USB radio final Status/Problem either way
            if ($usbDevice) {
                Write-Host ("USB RADIO FINAL STATE: InstanceId={0}; Status={1}; Problem={2}; ProblemStatus={3}; Service={4}" -f `
                    $usbDevice.InstanceId, $usbDevice.Status, $usbDevice.Problem, $usbDevice.ProblemStatus, $usbDevice.Service)
            } else {
                Write-Bad 'USB RADIO NOT FOUND within 60s matching USB\VID_0CF3&PID_6390*'
            }

            # Check Microsoft Bluetooth enumerators as evidence only (not part of verdict)
            $enumEvidence = Invoke-Bounded 'Query Microsoft Bluetooth enumerators' {
                $brb = @(Get-PnpDevice -InstanceId 'BTH\MS_BTHBRB*' -PresentOnly -ErrorAction SilentlyContinue).Count -gt 0
                $le = @(Get-PnpDevice -InstanceId 'BTH\MS_BTHLE*' -PresentOnly -ErrorAction SilentlyContinue).Count -gt 0
                [pscustomobject]@{ BrbPresent = $brb; LePresent = $le }
            }
            Write-Host ("MICROSOFT ENUMERATORS EVIDENCE: BTH\MS_BTHBRB (BR/EDR) Present={0}; BTH\MS_BTHLE (LE) Present={1}" -f $enumEvidence.BrbPresent, $enumEvidence.LePresent)

            # Bounded dumps:
            $btDevices = Invoke-Bounded 'Query Bluetooth class devices' {
                @(Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction SilentlyContinue | ForEach-Object {
                    [pscustomobject]@{
                        InstanceId   = $_.InstanceId
                        Status       = $_.Status
                        FriendlyName = $_.FriendlyName
                    }
                })
            }
            Write-Host 'PRESENT BLUETOOTH CLASS DEVICES:'
            if ($btDevices.Count -eq 0) {
                Write-Host '  (none)'
            } else {
                foreach ($dev in $btDevices) {
                    Write-Host ('  {0,-35} Status={1,-6} FriendlyName={2}' -f $dev.InstanceId, $dev.Status, $dev.FriendlyName)
                }
            }

            if ($usbDevice) {
                $usbProps = Invoke-Bounded 'Query USB radio device properties' {
                    param($UsbId)
                    @(Get-PnpDeviceProperty -InstanceId $UsbId -ErrorAction SilentlyContinue | Where-Object {
                        $_.KeyName -like '*Bluetooth*' -or ($_.KeyName -notlike 'DEVPKEY_Device_*')
                    } | ForEach-Object {
                        $formattedData = if ($_.Data -is [byte[]]) {
                            ([BitConverter]::ToString($_.Data) -replace '-')
                        } elseif ($_.Data -is [System.Array]) {
                            ($_.Data -join ', ')
                        } else {
                            "$($_.Data)"
                        }
                        [pscustomobject]@{
                            KeyName = $_.KeyName
                            Data    = $formattedData
                        }
                    })
                } @($usbDevice.InstanceId)
                Write-Host "USB RADIO PROPERTIES ($($usbDevice.InstanceId)):"
                if ($usbProps.Count -eq 0) {
                    Write-Host '  (none)'
                } else {
                    foreach ($prop in $usbProps) {
                        Write-Host ('  {0}: {1}' -f $prop.KeyName, $prop.Data)
                    }
                }
            }

            $freshValues = Get-ProbeValues
            Show-Ep0Trace $freshValues

            Write-Host 'BRIDGE RESULTS'
            if ($freshValues) {
                $freshValues.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } |
                    Sort-Object Name | ForEach-Object { Write-Host ('  {0,-30}: {1}' -f $_.Name, $_.Value) }
            }

            # 6. Verdict
            $bridgePass = ($null -ne $usbDevice) -and ($usbDevice.Status -eq 'OK') -and
                $enumsHealthy -and
                ($null -ne $freshValues) -and
                ($freshValues.UartBridgeCommands -gt 0) -and
                ($freshValues.UartBridgeEventsQueued -gt 0) -and
                ($freshValues.UartBridgeCommandsFailed -eq 0) -and
                ($freshValues.UartCompletion -eq 0)

            if ($bridgePass) {
                $script:verdict = 'PASS: BTHPORT initialized on the real controller'
                Write-Host $script:verdict -ForegroundColor Green
                # Native Windows discovery (tools\bt-scan.ps1) through this radio, plus
                # the controller events it produced. Evidence only; the run's exit code stays the verdict.
                try {
                    $scanPath = Join-Path $toolsDir 'bt-scan.ps1'
                    $scan = Invoke-Bounded 'Discovery scan (tools\bt-scan.ps1)' {
                        param($Path)
                        (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Path -TimeoutSeconds 60 2>&1 | Out-String)
                    } @($scanPath) 90
                } catch {
                    # A user-mode discovery process that overran is not a kernel PnP wedge: keep
                    # the bridge verdict and the bounded teardown.
                    $scan = "scan did not complete: $($_.Exception.Message)"
                    $script:wedged = $false
                    $script:primaryExit = 0
                    $script:verdict = 'PASS: BTHPORT initialized on the real controller'
                }
                Write-Host 'DISCOVERY EVIDENCE (discovery through this radio):'
                Write-Host (@($scan) -join "`n")
                $afterScan = Get-ProbeValues
                if ($afterScan) {
                    Write-Host ("DISCOVERY EVIDENCE: BridgeEventsQueued {0} -> {1}; BridgeEventsReceived {2} -> {3}" -f
                        $freshValues.UartBridgeEventsQueued, $afterScan.UartBridgeEventsQueued,
                        $freshValues.UartBridgeEventsReceived, $afterScan.UartBridgeEventsReceived)
                    Show-Ep0Trace $afterScan
                    Show-EventTrace $afterScan
                    Show-AdvSeen $afterScan
                }
                $unpaired = if ($scan -match 'unpaired=(\d+)') { [int]$matches[1] } else { 0 }
                $advGrew = ($afterScan -and $freshValues -and ($afterScan.UartAdvReports -gt $freshValues.UartAdvReports))
                if ($unpaired -gt 0 -and $advGrew) {
                    Write-Host ("DISCOVERY VERDICT: PASS ({0} unpaired devices heard through this radio; AdvReports {1} -> {2})" -f `
                        $unpaired, [int]$freshValues.UartAdvReports, [int]$afterScan.UartAdvReports) -ForegroundColor Green
                } else {
                    Write-Host ("DISCOVERY VERDICT: FAIL (unpaired={0}; AdvReports {1} -> {2})" -f `
                        $unpaired, [int]$freshValues.UartAdvReports, [int]$afterScan.UartAdvReports) -ForegroundColor Red
                }

                if ($HoldSeconds -gt 0) {
                    . (Join-Path $toolsDir 'bt-hid-watch.ps1')
                    $hidWatcher = New-BtHidWatcher
                    try {
                        $hidWatcher.Start()
                    } catch {
                        # Pairing evidence does not need it; never let it turn a bridge PASS into a FAIL.
                        Write-Bad ("HID watcher not running ({0}); no HID report evidence this run." -f $_.Exception.Message)
                        $hidWatcher = $null
                    }
                    try {
                        Write-Host ("Bridge held for up to {0} s. Perform the physical steps now; press Enter here to end early." -f $HoldSeconds)
                        $holdStart = [DateTime]::UtcNow
                        $holdDeadline = $holdStart.AddSeconds($HoldSeconds)
                        $lastSample = [DateTime]::MinValue
                        while ([DateTime]::UtcNow -lt $holdDeadline) {
                            $keyHit = $false
                            try {
                                if ([Console]::KeyAvailable) {
                                    $k = [Console]::ReadKey($true)
                                    if ($k.Key -eq [ConsoleKey]::Enter) { $keyHit = $true }
                                }
                            } catch { }
                            if ($keyHit) {
                                Write-Host '[*] Enter pressed; ending hold early.'
                                break
                            }
                            if (([DateTime]::UtcNow - $lastSample).TotalSeconds -ge 5) {
                                $lastSample = [DateTime]::UtcNow
                                $elapsed = [Math]::Floor(($lastSample - $holdStart).TotalSeconds)
                                $curValues = Get-ProbeValues
                                $curAclOut = if ($curValues -and $null -ne $curValues.UartBridgeAclOut) { $curValues.UartBridgeAclOut } else { 0 }
                                $curAclIn  = if ($curValues -and $null -ne $curValues.UartBridgeAclIn)  { $curValues.UartBridgeAclIn }  else { 0 }
                                $curEv     = if ($curValues -and $null -ne $curValues.UartBridgeEventsReceived) { $curValues.UartBridgeEventsReceived } else { 0 }
                                $curAdv    = if ($curValues -and $null -ne $curValues.UartAdvReports) { $curValues.UartAdvReports } else { 0 }
                                $curBtHid  = if ($hidWatcher) { $hidWatcher.TotalBluetoothInput() } else { 'n/a' }
                                $curScoIn  = if ($curValues -and $null -ne $curValues.UartBridgeScoIn) { $curValues.UartBridgeScoIn } else { 0 }
                                $curScoOut = if ($curValues -and $null -ne $curValues.UartBridgeScoOut) { $curValues.UartBridgeScoOut } else { 0 }
                                $curAlt    = if ($curValues -and $null -ne $curValues.ScoAltSetting) { $curValues.ScoAltSetting } else { 0 }
                                Write-Host ("[{0,3}s] AclOut={1} AclIn={2} EventsRx={3} AdvReports={4} BtHidTotal={5} ScoAlt={6} ScoIn={7} ScoOut={8}" -f `
                                    $elapsed, $curAclOut, $curAclIn, $curEv, $curAdv, $curBtHid, $curAlt, $curScoIn, $curScoOut)
                            }
                            Start-Sleep -Milliseconds 250
                        }
                    } finally {
                        if ($hidWatcher) { $hidWatcher.Stop() }
                    }

                    $holdEndBtDevices = Invoke-Bounded 'Query Bluetooth class devices' {
                        @(Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction SilentlyContinue | ForEach-Object {
                            [pscustomobject]@{
                                InstanceId   = $_.InstanceId
                                Status       = $_.Status
                                FriendlyName = $_.FriendlyName
                            }
                        })
                    }
                    Write-Host 'PRESENT BLUETOOTH CLASS DEVICES AFTER HOLD:'
                    if ($holdEndBtDevices.Count -eq 0) {
                        Write-Host '  (none)'
                    } else {
                        foreach ($dev in $holdEndBtDevices) {
                            Write-Host ('  {0,-35} Status={1,-6} FriendlyName={2}' -f $dev.InstanceId, $dev.Status, $dev.FriendlyName)
                        }
                    }

                    $paired = @(Get-DeckPairedDevices | Where-Object { $_ })
                    Write-Host ("PAIRED BLUETOOTH DEVICES ({0}):" -f $paired.Count)
                    if ($paired.Count -eq 0) {
                        Write-Host '  (none)'
                    } else {
                        foreach ($dev in $paired) {
                            Write-Host ('  {0,-35} FriendlyName={1}' -f $dev.InstanceId, $dev.FriendlyName)
                        }
                    }

                    $holdEndValues = Get-ProbeValues
                    Show-Ep0Trace $holdEndValues
                    Show-EventTrace $holdEndValues
                    Show-AdvSeen $holdEndValues
                    Show-ScoPath $holdEndValues

                    Write-Host 'RECORD RESULTS:'
                    if ($holdEndValues) {
                        $holdEndValues.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } |
                            Sort-Object Name | ForEach-Object { Write-Host ('  {0,-30}: {1}' -f $_.Name, $_.Value) }
                    }

                    $snap = if ($hidWatcher) { $hidWatcher.Snapshot() } else { @{} }
                    Write-Host ("CAPTURED RAW INPUT COUNTS ({0} devices):" -f $snap.Count)
                    $btDevicesWithInput = @()
                    $btInputTotal = 0
                    foreach ($kv in $snap.GetEnumerator()) {
                        $isBt = Test-BtHidDevice $kv.Key
                        Write-Host ("  {0}: {1} (Bluetooth={2})" -f $kv.Key, $kv.Value, $isBt)
                        if ($isBt) {
                            $btInputTotal += $kv.Value
                            if ($kv.Value -gt 0) {
                                $btDevicesWithInput += $kv.Key
                            }
                        }
                    }

                    # Count connections and encryption changes from EventLog
                    $connCount = 0
                    $encCount = 0
                    if ($holdEndValues -and $holdEndValues.EventCount -gt 0 -and $holdEndValues.EventLog) {
                        $eLog = [byte[]]$holdEndValues.EventLog
                        $eSlots = [Math]::Floor($eLog.Length / 8)
                        $eTotal = [int]$holdEndValues.EventCount
                        $eFirst = [Math]::Max(0, $eTotal - $eSlots)
                        for ($i = $eFirst; $i -lt $eTotal; $i++) {
                            $o = ($i % $eSlots) * 8
                            $c = $eLog[$o]
                            $p = $eLog[($o + 2)..($o + 7)]
                            if ($c -eq 0x03 -and $p[0] -eq 0) { $connCount++ }
                            if ($c -eq 0x3E -and ($p[0] -in @(0x01, 0x0A)) -and $p[1] -eq 0) { $connCount++ }
                            if ($c -eq 0x08 -and $p[0] -eq 0 -and $p[3] -ne 0) { $encCount++ }
                        }
                    }
                    $aclOut = if ($holdEndValues -and $null -ne $holdEndValues.UartBridgeAclOut) { $holdEndValues.UartBridgeAclOut } else { 0 }
                    $aclIn  = if ($holdEndValues -and $null -ne $holdEndValues.UartBridgeAclIn)  { $holdEndValues.UartBridgeAclIn }  else { 0 }
                    Write-Host ("PAIRING EVIDENCE: connections={0} encryption={1} aclOut={2} aclIn={3}" -f $connCount, $encCount, $aclOut, $aclIn)

                    $deviceNamesStr = if ($btDevicesWithInput.Count -gt 0) { $btDevicesWithInput -join ', ' } else { 'none' }
                    Write-Host ("HID REPORT EVIDENCE: {0} input reports from Bluetooth HID device(s): {1}" -f $btInputTotal, $deviceNamesStr)

                    $scoLinks = if ($holdEndValues -and $null -ne $holdEndValues.ScoLinkEvents) { $holdEndValues.ScoLinkEvents } else { 0 }
                    $scoField = { param($n) if ($holdEndValues -and $null -ne $holdEndValues.$n) { $holdEndValues.$n } else { 0 } }
                    Write-Host ("SCO VOICE EVIDENCE: syncLinks={0} alt={1} usbOut={2}B/{3}pkts toController={4} fromController={5} usbIn={6}B framed={7} lost={8}" -f
                        $scoLinks, (& $scoField 'ScoAltSetting'), (& $scoField 'ScoOutBytes'), (& $scoField 'ScoOutHciPackets'),
                        (& $scoField 'UartBridgeScoOut'), (& $scoField 'UartBridgeScoIn'), (& $scoField 'ScoInBytes'),
                        (& $scoField 'ScoInHciPackets'), (& $scoField 'UartBridgeScoLost'))
                }
            } else {
                $script:primaryExit = 1
                $usbStatus = if ($usbDevice) { $usbDevice.Status } else { 'Missing' }
                $brbStatus = if ($enums -and $enums.BrbPresent) { $enums.BrbStatus } else { 'Absent' }
                $leStatus = if ($enums -and $enums.LePresent) { $enums.LeStatus } else { 'Absent' }
                $cmdCount = if ($freshValues -and $null -ne $freshValues.UartBridgeCommands) { $freshValues.UartBridgeCommands } else { 'n/a' }
                $evCount = if ($freshValues -and $null -ne $freshValues.UartBridgeEventsQueued) { $freshValues.UartBridgeEventsQueued } else { 'n/a' }
                $failCount = if ($freshValues -and $null -ne $freshValues.UartBridgeCommandsFailed) { $freshValues.UartBridgeCommandsFailed } else { 'n/a' }
                $comp = if ($freshValues -and $null -ne $freshValues.UartCompletion) { $freshValues.UartCompletion } else { 'n/a' }
                $script:verdict = "FAIL: Bridge criteria not met (UsbStatus=$usbStatus, BthBrb=$brbStatus, BthLe=$leStatus, Commands=$cmdCount, EventsQueued=$evCount, CommandsFailed=$failCount, Completion=$comp)"
                Write-Bad $script:verdict
            }
        } else {
            # --- Probe Session ---
            $deadline = [DateTime]::UtcNow.AddSeconds($probeWaitSeconds)
            do {
                $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)))
                $node = Get-Target -TimeoutSeconds $remaining
                Assert-DeckNoDeviceProblem $node
                if ([DateTime]::UtcNow -ge $deadline) { break }
                $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)))
                $values = Get-ProbeValues -TimeoutSeconds $remaining
                if ($null -ne $values -and $values.UartCompletion -eq 2) {
                    $script:wedged = $true
                    $script:primaryExit = 1
                    $script:verdict = "FAIL: driver reported unconfirmed retirement (UartCompletion=2); UART work may remain active in kernel."
                    # Render the snapshot already fetched; never issue diagnostic reads after this marker.
                    $values.PSObject.Properties | Where-Object {
                        $_.Name -like 'Uart*' -or $_.Name -in @('LastAddDeviceStep', 'LastAddDeviceStatus')
                    } | Sort-Object Name | ForEach-Object {
                        Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value)
                    }
                    throw $script:verdict
                }
                if ($node.Service -eq $driverService -and $values.UartProbeRan -eq 1 -and $values.UartCompletion -eq 1) { break }
                Start-Sleep -Milliseconds 500
            } while ([DateTime]::UtcNow -lt $deadline)
            if ($node.Service -ne $driverService -or $values.UartProbeRan -ne 1 -or $values.UartCompletion -ne 1) {
                $script:wedged = $true
                $script:primaryExit = 1
                $script:verdict = "FAIL: probe did not record confirmed completion (Ran=1, Completion=1) on service $driverService within ${probeWaitSeconds}s; ownership retirement unconfirmed."
                throw $script:verdict
            }
            Write-Host 'UART PROBE RESULTS'
            $values.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } | Sort-Object Name | ForEach-Object { Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value) }
            $pass = ($values.UartProbeRan -eq 1) -and ($values.UartCompletion -eq 1) -and
                ($values.UartSerialOpened -eq 1) -and ($values.UartPatchBytesSent -gt 0) -and
                ($values.UartNvmBytesSent -gt 0) -and ($values.UartHciResetSent -eq 1) -and
                ($values.UartHciResetStatus -eq 0) -and ($values.UartHciResetEventLen -gt 0) -and
                ($values.UartAborted -ne 1)
            if ($pass) { $script:verdict = 'PASS: firmware and HCI_Reset' }
            else {
                $script:primaryExit = 1
                $script:verdict = "FAIL: $($values.UartFailurePhase); step=$($values.UartLastStep); status=$($values.UartLastStatus); aborted=$($values.UartAborted); elapsedMs=$($values.UartElapsedMs)"
            }
        }
    }
} catch {
    if ($script:primaryExit -eq 0) { $script:primaryExit = 1 }
    $script:verdict = "FAIL: $($_.Exception.Message)"
    Write-Bad $script:verdict
} finally {
    try {
        if ($script:wedged) { Show-Recovery }
        elseif ($script:mutated) {
            if ($Bridge) {
                Invoke-Bounded 'Set Enabled=0' { param($Key) Set-ItemProperty -LiteralPath $Key -Name Enabled -Value 0 -Type DWord } @($paramsKey)
            } else {
                Invoke-Bounded 'Clear UartProbe' { param($Key) Remove-ItemProperty -LiteralPath $Key -Name UartProbe -ErrorAction SilentlyContinue } @($paramsKey)
            }
            if ($KeepBound) {
                Write-Host 'DeckBtUsb stays bound and serves the radio. tools\session.ps1 -Stop hands it back to the stock driver.' -ForegroundColor Green
            } else {
                if ($Bridge) {
                    $bthWasRunning = Invoke-Bounded 'Query and stop bthserv' {
                        $svc = Get-Service bthserv -ErrorAction SilentlyContinue
                        $wasRunning = ($null -ne $svc -and $svc.Status -eq 'Running')
                        if ($wasRunning) {
                            Stop-Service bthserv -Force
                        }
                        return $wasRunning
                    }
                }
                $packages = @(Get-ProbePackages)
                if ($packages.Count -and -not $script:stagedOemInf) {
                    throw 'Cannot prove package ownership for this run; refusing package deletion.'
                }
                $packages = @($packages | Where-Object { $_.Published -eq $script:stagedOemInf })
                $uninstallReboot = $false
                foreach ($p in $packages) {
                    $uResult = Invoke-Pnp @('/delete-driver', $p.Published, '/uninstall', '/force')
                    if ($uResult.Code -eq 3010 -or $uResult.Output -match '(?i)reboot') {
                        $uninstallReboot = $true
                    }
                }
                if ($Bridge -and $uninstallReboot) {
                    $script:primaryExit = 1
                    $script:verdict = 'REBOOT REQUIRED: driver package removal requires a system restart.'
                    Write-Bad 'REBOOT REQUIRED: driver package removal requires a system restart.'
                    Write-Host 'Exact next steps after reboot:'
                    Write-Host '  1. Reboot the system.'
                    Write-Host '  2. Run tools\verify-clean.ps1 (elevated) to verify clean vendor state.'
                } else {
                    if ($Bridge) {
                        $releaseDeadline = [DateTime]::UtcNow.AddSeconds($releaseWaitSeconds)
                        $released = $false
                        do {
                            $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($releaseDeadline - [DateTime]::UtcNow).TotalSeconds)))
                            $vals = Get-ProbeValues -TimeoutSeconds $remaining
                            if ($null -ne $vals) {
                                if ($vals.UartCompletion -eq 2) {
                                    $script:wedged = $true
                                    $script:primaryExit = 1
                                    $script:verdict = 'FAIL: driver reported unconfirmed retirement (UartCompletion=2) during teardown.'
                                    Show-Recovery
                                    throw $script:verdict
                                }
                                if ($vals.UartCompletion -eq 1) {
                                    $released = $true
                                    break
                                }
                            }
                            if ([DateTime]::UtcNow -ge $releaseDeadline) { break }
                            Start-Sleep -Milliseconds 500
                        } while ([DateTime]::UtcNow -lt $releaseDeadline)

                        if (-not $released) {
                            $script:wedged = $true
                            $script:primaryExit = 1
                            $script:verdict = 'FAIL: driver retirement (Completion=1) unconfirmed within 30s during teardown.'
                            Show-Recovery
                            throw $script:verdict
                        }

                        Write-Host "Teardown driver release confirmed: UartFailurePhase=$($vals.UartFailurePhase); HandbackBaud=$($vals.UartHandbackBaud); HandbackStatus=$($vals.UartHandbackStatus)"
                        if ($vals.UartHandbackBaud -ne 115200) {
                            Write-Bad "WARNING: HandbackBaud is $($vals.UartHandbackBaud) (expected 115200). Vendor driver may fail to initialize until reboot."
                        }

                        Invoke-Bounded 'Remove Enabled parameter' { param($Key) Remove-ItemProperty -LiteralPath $Key -Name Enabled -ErrorAction SilentlyContinue } @($paramsKey)
                    }
                        if (-not (Test-Path -LiteralPath $vendorInfStore)) { [void](Invoke-Pnp @('/add-driver', $vendorInfBackup, '/install')) }
                        $node = Get-Target
                        if (-not $node) { throw 'Radio missing during restore.' }
                        # Package /uninstall already rebinds the vendor.
                        # Windows Home rejects enabling an enabled device (exit 50), and a restart is
                        # vetoed while Bluetooth child stacks are open; same contract as identify.
                        if ("$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')) {
                            $result = Invoke-Pnp @('/enable-device', $node.InstanceId)
                            if ($result.Code -eq 3010) { throw 'Enabling the vendor radio requires reboot; stop and report.' }
                        }
                        [void](Invoke-Pnp @('/scan-devices'))
                        $deadline = [DateTime]::UtcNow.AddSeconds(30)
                        $healthy = $false
                        do {
                            $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)))
                            $node = Get-Target -TimeoutSeconds $remaining
                            Assert-DeckNoDeviceProblem $node
                            if ([DateTime]::UtcNow -ge $deadline) { break }
                            $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)))
                            $child = Get-Target $RadioChildLike -TimeoutSeconds $remaining
                            Assert-DeckNoDeviceProblem $child
                            if ($node.Service -eq $VendorService -and $node.Status -eq 'OK' -and $child.Status -eq 'OK') {
                                $healthy = $true
                                break
                            }
                            if ([DateTime]::UtcNow -ge $deadline) { break }
                            Start-Sleep -Milliseconds 500
                        } while ([DateTime]::UtcNow -lt $deadline)
                        if (-not $healthy -or [DateTime]::UtcNow -ge $deadline) {
                            throw 'Vendor ownership and healthy Bluetooth child not verified within 30s.'
                        }
                        $remaining = [Math]::Min($PnpTimeoutSeconds, [Math]::Max(10, [Math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)))
                        $restoredState = Invoke-Bounded 'Verify vendor radio health' { Assert-DeckHealthyVendorRadio } @() $remaining | Select-Object -First 1
                        if ($null -eq $restoredState) { throw 'Vendor health verification returned no result.' }
                        Write-Host 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy.'
                    if ($Bridge -and $bthWasRunning) {
                        Invoke-Bounded 'Restart bthserv' {
                            Start-Service bthserv -ErrorAction SilentlyContinue
                        }
                    }
                }
            }
        }
    } catch {
        $script:primaryExit = 1
        $script:verdict = "FAIL: restore not verified; $($_.Exception.Message)"
        Write-Bad "RESTORE FAILED: $($_.Exception.Message)"
        Show-Recovery
    }
    Write-Host "THIS RUN: $($script:verdict); exit=$($script:primaryExit); log=$LogPath"
    if ($script:transcriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
}
exit $script:primaryExit
