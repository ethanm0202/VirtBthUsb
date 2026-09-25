<#
    uart-identify.ps1 - radio identify operator. Every device, package and registry operation
    runs in a disposable PowerShell process with a hard wall-clock deadline. Killing that process
    does not cancel a wedged kernel IRP: after a timeout no further state access is attempted.
    This probe downloads no firmware, issues no resets, and emits only a read-only
    vendor version request (01 00 FC 01 19) to detect controller identity and operating baud.
    -RestoreOnly selects post-reboot vendor recovery instead of identification; use tools\session.ps1 -Stop.
    Its -DryRun validates the saved backup only and never queries devices, services, or registry.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $DryRun,
    [switch] $Force,
    [switch] $KeepBound,
    [ValidateRange(1, 600)][int] $PnpTimeoutSeconds = 60,
    [string] $LogPath,
    [switch] $RestoreOnly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'deck-state.ps1')
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

function Invoke-Pnp([string[]] $Arguments, [int[]] $AcceptedExitCodes = @(0, 3010)) {
    $r = Invoke-Bounded ('pnputil ' + ($Arguments -join ' ')) {
        param($NativeArgs)
        Invoke-DeckNative pnputil.exe $NativeArgs
    } @(,$Arguments)
    Write-Host $r.Output
    if ($r.Code -notin $AcceptedExitCodes) { throw "pnputil failed (exit $($r.Code)): $($Arguments -join ' ')" }
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
    Invoke-Bounded 'Read identify registry results' {
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

function Show-Recovery {
    Write-Bad 'Automatic restore is not verified. Stop device and service operations; preserve this transcript.'
    if ($script:wedged) {
        Write-Host 'STOP: an operation timed out or driver retirement is unconfirmed. A responsive desktop does not prove UART release.'
        Write-Host 'Do not rerun identify, uninstall the driver, or run full rollback in this boot.'
        Write-Host "Preserve this transcript. Staged package: $($script:stagedOemInf); service: $driverService."
        Write-Host 'Save work if possible. A restart may hang while PnP is wedged; if Windows stops responding, a hard power-off may be required.'
        Write-Host 'After a successful boot, use tools\session.ps1 -Stop once from an elevated console; if it fails, stop and preserve its transcript.'
        return
    }
    Write-Host 'After the machine is responsive again, run these commands from an elevated console:'
    Write-Host "  reg delete HKLM\SYSTEM\CurrentControlSet\Services\$driverService\Parameters /v UartIdentify /f"
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

function Get-RadioRestorePlan {
    # Validate the saved recovery payload before removing any installed package.
    $manifest = @(Get-Content -LiteralPath (Join-Path $DeckBaselineDir 'MANIFEST.sha256'))
    $files = @('baseline.json', 'vendor-qcbtuart/qcbtuart.inf', 'vendor-qcbtuart/qcbtuart.sys',
        'vendor-qcbtuart/qcbtuart.cat', 'vendor-qcbtuart/hpbtfw21.tlv',
        'vendor-qcbtuart/hpnv21.bin', 'vendor-qcbtuart/hpnv21g.bin',
        'vendor-qcbtuart/hpnv21.309', 'vendor-qcbtuart/hpnv21g.309')
    foreach ($relative in $files) {
        $pattern = '^([0-9a-fA-F]{64})  ' + [regex]::Escape($relative) + '$'
        $entry = @($manifest | Where-Object { $_ -match $pattern })
        if ($entry.Count -ne 1) { throw "Recovery manifest entry missing or ambiguous: $relative" }
        $expected = [regex]::Match($entry[0], $pattern).Groups[1].Value
        $actual = (Get-FileHash -LiteralPath (Join-Path $DeckBaselineDir $relative) -Algorithm SHA256).Hash
        if ($actual -ine $expected) { throw "Recovery backup hash mismatch: $relative" }
    }
    $baseline = Get-Content -LiteralPath $DeckBaselineJson -Raw | ConvertFrom-Json
    $radio = $baseline.qcom2066
    if ($radio.service -ne 'QcBluetooth' -or $radio.childService -ne 'BthMini' -or
        $radio.instanceId -notlike 'ACPI\QCOM2066\*' -or $radio.childInstanceId -notlike 'QCA_SHB\UART_H4\*') {
        throw 'Recovery baseline does not identify the expected Qualcomm radio and child.'
    }
    return [pscustomobject]@{
        Radio = $radio.instanceId; Child = $radio.childInstanceId; VendorInf = $vendorInfBackup
    }
}

function Get-RadioRestorePackages {
    @(Invoke-Bounded 'Read recovery package inventory' {
        $result = Invoke-DeckNative pnputil.exe @('/enum-drivers')
        if ($result.Code -ne 0) { throw "Package enumeration failed (exit $($result.Code))." }
        Get-DeckPackageList $result.Output
    })
}

function Assert-RadioRestored($Plan) {
    $health = Invoke-Bounded 'Verify restored vendor radio and child' { Assert-DeckHealthyVendorRadio }
    if ($null -eq $health -or $health.Radio -ine $Plan.Radio -or $health.Child -ine $Plan.Child) {
        throw 'Restored device identity does not match the saved baseline.'
    }
    Write-Host 'RADIO RESTORE VERIFIED: QcBluetooth OK; child BthMini OK. No probe was armed.'
}

function Invoke-RadioRestore($Plan) {
    $node = Get-Target
    if ($null -eq $node -or $node.InstanceId -ine $Plan.Radio) {
        throw 'Radio missing or different from the saved baseline; preserving all packages.'
    }
    if ($node.Service -notin @('DeckBtUsb', 'QcBluetooth')) {
        throw "Unexpected radio owner '$($node.Service)'; preserving all packages."
    }
    $disabled = "$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')
    $claiming = $null
    if ($node.Service -eq 'QcBluetooth' -and -not $disabled) {
        Assert-RadioRestored $Plan
        return
    }
    if ($node.Service -eq 'DeckBtUsb') {
        $packages = @(Get-RadioRestorePackages | Where-Object { $_.OriginalName -ieq 'deckbtusb.inf' })
        if ($packages.Count -ne 1 -or -not $packages[0].IsOurs -or
            $packages[0].Published -notmatch '^oem[0-9]+\.inf$') {
            throw 'Claiming project package missing, foreign, or ambiguous; preserving all packages.'
        }
        $claiming = $packages[0].Published
        Invoke-Bounded 'Verify exclusive package binding and disarmed driver' {
            param($Id, $Published, $Key)
            if ((Get-DeckNodeProperty $Id DEVPKEY_Device_DriverInfPath) -ine $Published) {
                throw 'Radio binding does not match the owned package; refusing removal.'
            }
            $users = @(Get-CimInstance Win32_PnPSignedDriver -Filter "InfName='$Published'" -ErrorAction Stop)
            if ($users.Count -ne 1 -or $users[0].DeviceID -ine $Id) {
                throw 'The package has missing, ambiguous, or other device bindings; refusing package-wide removal.'
            }
            if (Test-Path -LiteralPath $Key) {
                $values = Get-ItemProperty -LiteralPath $Key -ErrorAction Stop
                foreach ($name in @('Enabled', 'UartIdentify')) {
                    $value = $values.PSObject.Properties[$name]
                    if ($null -ne $value -and $value.Value -ne 0) {
                        throw "Driver token $name is not disarmed; stop rather than interrupt a possible probe."
                    }
                }
            }
        } @($Plan.Radio, $claiming, $paramsKey)
        Write-Host "Recovery target: $($Plan.Radio); remove only $claiming [deckbtusb.inf, DeckBtUsb]."
    } else {
        Write-Host "Recovery target: vendor-owned $($Plan.Radio) is disabled; enable only."
    }
    if (-not $Force -and (Read-Host 'After a successful Windows restart, type RESTORE RADIO to proceed') -ne 'RESTORE RADIO') {
        throw 'Aborted by operator. Nothing was changed.'
    }
    if ($claiming) {
        # Stage first so a verified vendor package is available before the claiming package goes away.
        $result = Invoke-Pnp @('/add-driver', $Plan.VendorInf)
        if ($result.Code -eq 3010) { throw 'Vendor staging requires reboot; stop and report before further operations.' }
        $result = Invoke-Pnp @('/delete-driver', $claiming, '/uninstall')
        if ($result.Code -eq 3010) { throw 'Package removal requires reboot; stop and report before further operations.' }
        if (@(Get-RadioRestorePackages | Where-Object { $_.Published -ieq $claiming }).Count) {
            throw 'The claiming package remains staged; restoration is not verified.'
        }
        # An already-current driver may return ERROR_NO_MORE_ITEMS; binding/health below decide success.
        $result = Invoke-Pnp @('/add-driver', $Plan.VendorInf, '/install') @(0, 259, 3010)
        if ($result.Code -eq 3010) { throw 'Vendor installation requires reboot; stop and report before further operations.' }
        $node = Get-Target
        if ($null -eq $node -or $node.InstanceId -ine $Plan.Radio -or $node.Service -ne 'QcBluetooth') {
            throw 'The expected radio did not rebind to QcBluetooth; no enable/restart will be attempted.'
        }
        $disabled = "$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')
    }
    if ($disabled) {
        $result = Invoke-Pnp @('/enable-device', $Plan.Radio)
        if ($result.Code -eq 3010) { throw 'Enabling the vendor radio requires reboot; stop and report.' }
    }
    Assert-RadioRestored $Plan
}

if ($RestoreOnly) {
    try {
        if ($KeepBound) {
            throw '-RestoreOnly cannot be combined with keep-bound option.'
        }
        Write-Host 'RADIO RESTORE ONLY - no identification, firmware probing, or security changes.'
        Write-Host 'Use only after a completed Windows restart. Stop on any failure; do not stack recovery commands.'
        $plan = Get-RadioRestorePlan
        Write-Host "Verified recovery backup; target $($plan.Radio); vendor INF $($plan.VendorInf)"
        if ($DryRun) {
            Write-Host 'DRY-RUN: validate backup only; no device/service/registry access or mutation.'
            Write-Host 'Live plan: verify ownership and idle tokens; stage vendor; remove only the exclusively bound project package; install vendor; enable only if disabled; verify radio and child.'
        } else {
            if (-not (Test-DeckElevated)) { throw 'Administrator elevation required; use tools\session.ps1 -Stop.' }
            if (-not $LogPath) { $LogPath = Join-Path $PSScriptRoot ('_build\sessions\restore-radio-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss')) }
            [void](New-Item -ItemType Directory -Path (Split-Path -Parent $LogPath) -Force)
            Start-Transcript -LiteralPath $LogPath -Force | Out-Null
            $script:transcriptStarted = $true
            Invoke-RadioRestore $plan
        }
    } catch {
        $script:primaryExit = 1
        Write-Bad "RADIO RESTORE NOT VERIFIED: $($_.Exception.Message)"
        Write-Host 'STOP: no further device/service/registry operations will be attempted. Report this result; do not rerun identify or full rollback.'
    } finally {
        Write-Host "RADIO RESTORE exit=$($script:primaryExit); log=$LogPath"
        if ($script:transcriptStarted) { try { Stop-Transcript | Out-Null } catch { } }
    }
    exit $script:primaryExit
}

try {
    Write-Host 'DeckBtUsb radio identify operator (downloads no firmware)'
    Write-Host "Target: $targetId; DryRun=$([bool]$DryRun); PnP timeout=${PnpTimeoutSeconds}s"
    $probeWaitSeconds = 75
    $actionPlan = "stage $driverInf; install $driverInf; arm identify; enable disabled target OR restart already-enabled target once"
    $cleanupPlan = 'uninstall; verify restore'
    Write-Host "Plan: validate package; $actionPlan; wait up to ${probeWaitSeconds}s for recorded completion; report results; clear identify; $cleanupPlan."
    if ($DryRun) {
        $script:verdict = 'DRY-RUN: no mutation (including no log files or registry access)'
    } else {
        if (-not (Test-DeckElevated)) { $script:primaryExit = 2; throw 'Administrator elevation required; this script does not elevate itself.' }
        $requiredFiles = @($driverSysName, $driverInfName, $driverCatName)
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
            Write-Host 'Physical mode claims the Qualcomm radio to identify controller identity and baud rate. Downloads NO firmware.'
        # Precondition before mutation: verify the service is not an orphaned remnant.
        # A valueless service remnant (e.g. from an earlier uninstall that stripped values
        # but left the key because Parameters was inside it) breaks AddService with Error 2.
        # Check for this remnant upfront before any mutation and direct the owner to cleanup.
        $svcRecord = Invoke-Bounded 'Read identify service state' { param($Name) Get-DeckServiceRecord $Name } @($driverService) | Select-Object -First 1
        if (Test-DeckOwnedServiceRemnant $svcRecord) {
            $script:primaryExit = 2
            throw "REFUSAL: owned service remnant '$driverService' is present. Run tools\session.ps1 -Uninstall to clear it before starting."
        }
        if (-not $Force) {
            $prompt = 'Physical mode claims the radio (downloads no firmware). Type IDENTIFY to proceed'
            if ((Read-Host $prompt) -ne 'IDENTIFY') { throw 'Operator refused; nothing changed.' }
        }
        if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $root ('tools\_build\sessions\identify-{0}.log' -f (Get-Date -Format yyyyMMdd-HHmmss)) }
        [void](New-Item -ItemType Directory -Path (Split-Path -Parent ([IO.Path]::GetFullPath($LogPath))) -Force)
        Start-Transcript -Path $LogPath | Out-Null
        $script:transcriptStarted = $true
        $script:mutated = $true
        $script:packageAttempted = $true
        # Stage first, so the exact published name is known before a PnP start can wedge.
        [void](Invoke-Pnp @('/add-driver', $driverInf))
        $packages = @(Get-ProbePackages)
        if ($packages.Count -ne 1) { throw 'Cannot uniquely identify staged probe package; refusing install.' }
        $script:stagedOemInf = $packages[0].Published

        [void](Invoke-Pnp @('/add-driver', $driverInf, '/install'))
        $node = Get-Target
        if (-not $node) { throw 'Target missing after install.' }

        # Guard: verify the service key legitimately exists after /install. Never use New-Item -Force
        # in a way that can create a missing parent service key.
        Invoke-Bounded 'Clear stale results and arm UartIdentify' {
            param($Key)
            $serviceKey = Split-Path -Parent $Key
            if (-not (Test-Path -LiteralPath $serviceKey)) {
                throw "REFUSAL: service key '$serviceKey' does not exist after install; AddService may have failed."
            }
            if (-not (Test-Path -LiteralPath $Key)) {
                [void](New-Item -Path $Key)
            }
            $old = Get-ItemProperty -LiteralPath $Key
            foreach ($p in $old.PSObject.Properties) {
                if ($p.Name -like 'Uart*') { Remove-ItemProperty -LiteralPath $Key -Name $p.Name }
            }
            Set-ItemProperty -LiteralPath $Key -Name UartIdentify -Value 1 -Type DWord
        } @($paramsKey)

        $startAction = if ("$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')) { '/enable-device' } else { '/restart-device' }
        [void](Invoke-Pnp @($startAction, $node.InstanceId))
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
            if ($node.Service -eq $driverService -and $values.UartIdentifyRan -eq 1 -and $values.UartCompletion -eq 1) { break }
            Start-Sleep -Milliseconds 500
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($node.Service -ne $driverService -or $values.UartIdentifyRan -ne 1 -or $values.UartCompletion -ne 1) {
            $script:wedged = $true
            $script:primaryExit = 1
            $script:verdict = "FAIL: identify did not record confirmed completion (Ran=1, Completion=1) on service $driverService within ${probeWaitSeconds}s; ownership retirement unconfirmed."
            throw $script:verdict
        }

        Write-Host ''
        Write-Host '========================================================================'
        Write-Host ' UART IDENTIFY RESULTS'
        Write-Host '========================================================================'
        $values.PSObject.Properties | Where-Object { $_.Name -like 'Uart*' } | Sort-Object Name | ForEach-Object {
            Write-Host ('  {0,-28}: {1}' -f $_.Name, $_.Value)
        }
        Write-Host '------------------------------------------------------------------------'

        $attemptsVal = if ($null -ne $values.UartIdentifyAttempts) { [uint32]$values.UartIdentifyAttempts } else { [uint32]0 }
        $hasLastStatus = ($null -ne $values.UartLastStatus)
        $lastStatusVal = if ($hasLastStatus) { [int64]$values.UartLastStatus -band 0xFFFFFFFFL } else { [int64]0 }
        $hasAborted = ($null -ne $values.UartAborted)
        $abortedVal = if ($hasAborted) { [int]$values.UartAborted } else { 0 }

        $answered = ($values.UartIdentifyRan -eq 1) -and
                    ($values.UartCompletion -eq 1) -and
                    ($null -ne $values.UartIdentifyBaud -and $values.UartIdentifyBaud -gt 0) -and
                    (($values.UartFailurePhase -eq 'Answered') -and ($hasLastStatus -and $lastStatusVal -eq 0)) -and
                    ($hasAborted -and $abortedVal -eq 0)

        $isMissingSerialNotAttempted = ($values.UartFailurePhase -eq 'MissingSerialResource' -and $attemptsVal -eq 0)
        $isNoResponse = ($values.UartIdentifyRan -eq 1 -and
                         ($values.UartFailurePhase -eq 'NoResponse' -and $hasLastStatus -and $lastStatusVal -eq 3221226021 -and $hasAborted -and $abortedVal -eq 0))

        $answeringRate = if ($answered) {
            "{0} baud" -f $values.UartIdentifyBaud
        } elseif ($isMissingSerialNotAttempted) {
            "not attempted (missing serial resource)"
        } elseif ($isNoResponse) {
            "no rate answered (controller silent across ladder)"
        } elseif ($hasAborted -and $abortedVal -eq 1) {
            "aborted ($($values.UartFailurePhase); status=0x{0:X8})" -f $lastStatusVal
        } else {
            "transport error ($($values.UartFailurePhase); status=0x{0:X8})" -f $lastStatusVal
        }
        $socIdVal = if ($null -ne $values.UartSocId) { [uint32]$values.UartSocId } else { [uint32]0 }
        $romVerVal = if ($null -ne $values.UartRomVersion) { [uint32]$values.UartRomVersion } else { [uint32]0 }
        $productIdVal = if ($null -ne $values.UartProductId) { [uint32]$values.UartProductId } else { [uint32]0 }
        $patchVerVal = if ($null -ne $values.UartPatchVersion) { [uint32]$values.UartPatchVersion } else { [uint32]0 }
        $isGf = (($socIdVal -band 0xFF00) -eq 0x1200)
        $variant = if ($isGf) { 'g' } else { '' }
        $foundryName = if ($isGf) { 'g (GlobalFoundries)' } else { 'standard (non-GF)' }
        $nvmName = 'hpnv{0:x2}{1}.bin' -f $romVerVal, $variant

        Write-Host ('  Answering baud rate         : {0}' -f $answeringRate)
        Write-Host ('  Ladder attempts             : {0}' -f $attemptsVal)
        if ($answered) {
            Write-Host ('  Controller SoC ID           : 0x{0:X8} ({0})' -f $socIdVal)
            Write-Host ('  Controller ROM version      : 0x{0:X4} ({0})' -f $romVerVal)
            Write-Host ('  Product ID                  : 0x{0:X8}' -f $productIdVal)
            Write-Host ('  Patch version               : 0x{0:X4}' -f $patchVerVal)
            Write-Host ('  Derived foundry variant     : {0}' -f $foundryName)
            Write-Host ('  Derived NVM file (no board) : {0}' -f $nvmName)
            if ($values.UartIdentifyRawHex) {
                Write-Host ('  Raw response hex            : {0}' -f $values.UartIdentifyRawHex)
            }
        } else {
            $identityDesc = if ($isMissingSerialNotAttempted) {
                'unobserved (not attempted)'
            } elseif ($isNoResponse) {
                'unobserved (silent controller)'
            } elseif ($hasAborted -and $abortedVal -eq 1) {
                "unobserved (aborted: $($values.UartFailurePhase); status=0x{0:X8})" -f $lastStatusVal
            } else {
                "unobserved (transport error: $($values.UartFailurePhase); status=0x{0:X8})" -f $lastStatusVal
            }
            Write-Host ('  Controller identity         : {0}' -f $identityDesc)
            Write-Host ('  Derived foundry variant     : unobserved')
            Write-Host ('  Derived NVM file (no board) : unobserved')
        }
        Write-Host ''

        if ($answered) {
            $script:verdict = "PASS: controller identified at $answeringRate (soc_id=0x{0:X8}, rom_ver=0x{1:X4}, nvm=$nvmName)" -f $socIdVal, $romVerVal
        } else {
            $script:primaryExit = 1
            if ($isNoResponse) {
                $script:verdict = "FAIL: no rate answered (controller silent across ladder); attempts=$attemptsVal"
            } else {
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
            Invoke-Bounded 'Clear UartIdentify' { param($Key) if (Test-Path -LiteralPath $Key) { Remove-ItemProperty -LiteralPath $Key -Name UartIdentify -ErrorAction SilentlyContinue } } @($paramsKey)
            if ($KeepBound) {
                Write-Bad '-KeepBound: radio NOT restored. DeckBtUsb remains bound.'
                Show-Recovery
            } else {
                $packages = @(Get-ProbePackages)
                if ($packages.Count -and -not $script:stagedOemInf) {
                    throw 'Cannot prove package ownership for this run; refusing package deletion.'
                }
                $packages = @($packages | Where-Object { $_.Published -eq $script:stagedOemInf })
                foreach ($p in $packages) { [void](Invoke-Pnp @('/delete-driver', $p.Published, '/uninstall', '/force')) }
                    if (-not (Test-Path -LiteralPath $vendorInfStore)) { [void](Invoke-Pnp @('/add-driver', $vendorInfBackup, '/install')) }
                    $node = Get-Target
                    if (-not $node) { throw 'Radio missing during restore.' }
                    # Package /uninstall already rebinds the vendor.
                    # Windows Home rejects enabling an enabled device (exit 50), and a restart is
                    # vetoed while Bluetooth child stacks are open; same contract as -RestoreOnly.
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
