<#
    restore-radio.ps1 - hands the radio back to QcBluetooth after -KeepBound or
    re-enables the stock radio after -Prepare. Stops bthserv for package removal,
    confirms a fresh controller handback when a live bridge child existed, and verifies
    the vendor radio and child before restarting bthserv.
    Elevated. Exit 0 = verified vendor radio; 1 = not verified (including reboot pending);
    2 = elevation refusal. Stop on exit 1; do not stack recovery commands.
#>
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')
if (-not (Test-DeckElevated)) { Write-Host 'REFUSAL: run elevated.'; exit 2 }
$paramsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'

function Stop-BthservBounded([int] $TimeoutSeconds = 20) {
    if ((Get-Service bthserv).Status -eq 'Stopped') { return }
    Stop-Service bthserv -Force -NoWait
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline -and (Get-Service bthserv).Status -ne 'Stopped') { Start-Sleep -Milliseconds 500 }
    if ((Get-Service bthserv).Status -eq 'Stopped') { return }
    $svc = Get-CimInstance Win32_Service -Filter "Name='bthserv'"
    $shared = @(Get-CimInstance Win32_Service | Where-Object { $_.ProcessId -eq $svc.ProcessId -and $_.Name -ne 'bthserv' })
    if ($svc.ProcessId -and $shared.Count -eq 0) {
        Write-Host "[*] bthserv still $($svc.State); ending its dedicated host process $($svc.ProcessId)."
        Stop-Process -Id $svc.ProcessId -Force
        Start-Sleep -Seconds 3
    }
}

function Invoke-RestoreBounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments = @(), [int] $TimeoutSeconds = 60) {
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
    `$value = @(& ([scriptblock]::Create(`$p.Action)) @a)
    @{ Success = `$true; Value = `$value } | Export-Clixml -LiteralPath `$p.ResultPath
} catch {
    @{ Success = `$false; Error = `$_.Exception.Message } | Export-Clixml -LiteralPath `$p.ResultPath
}
"@
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PSHOME 'powershell.exe'
    $start.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "Could not start worker: $Label" }
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill() } catch { }
            throw "TIMEOUT: $Label exceeded ${TimeoutSeconds}s. Kernel PnP work may still be running."
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

function Invoke-RestorePnp([string[]] $Arguments) {
    $result = Invoke-RestoreBounded ('pnputil ' + ($Arguments -join ' ')) {
        param($NativeArgs)
        Invoke-DeckNative pnputil.exe $NativeArgs
    } @(,$Arguments)
    Write-Host $result.Output.Trim()
    if ($result.Code -eq 3010 -or $result.Output -match '(?i)reboot (?:is )?(?:needed|required)') {
        throw "REBOOT REQUIRED: pnputil $($Arguments[0]) is pending. Do not attempt further device or service operations in this boot."
    }
    if ($result.Code -ne 0) { throw "pnputil $($Arguments[0]) failed (exit $($result.Code)); no further operations attempted." }
}

$bthStart = $null
try {
    $radio = @(Invoke-RestoreBounded 'Read radio' { Get-DeckCheckedDevices 'ACPI\QCOM2066*' -PresentOnly })
    if ($radio.Count -ne 1) { throw "Expected one ACPI\QCOM2066 node, found $($radio.Count)." }
    $id = $radio[0].InstanceId
    $owner = Invoke-RestoreBounded 'Read radio owner' {
        param($Id)
        Get-DeckNodeProperty $Id DEVPKEY_Device_Service
    } @($id)
    $packages = @(Invoke-RestoreBounded 'Enumerate DeckBtUsb packages' {
        @(Get-DeckStagedPackages | Where-Object { $_.IsOurs -and $_.OriginalName -ieq 'deckbtusb.inf' })
    })
    Write-Host ("[*] radio {0} service={1} status={2}; DeckBtUsb packages: {3}" -f $id,
        $owner, $radio[0].Status, (($packages | ForEach-Object { $_.Published }) -join ', '))
    if ($packages.Count -gt 0) {
        if ($owner -notin @('QcBluetooth', 'DeckBtUsb')) { throw "Unexpected radio owner '$owner'; no package removed." }
        if ($packages.Count -ne 1 -or $packages[0].Published -notmatch '^oem[0-9]+\.inf$') {
            throw 'Project package inventory ambiguous; no package removed.'
        }
        $bridge = @(Invoke-RestoreBounded 'Read live bridge child' {
            Get-DeckCheckedDevices 'USB\VID_0CF3&PID_6390*' -PresentOnly
        })
        if ($bridge.Count -gt 1) { throw 'Bridge child identity ambiguous; no package removed.' }
        $active = $bridge.Count -eq 1
        if ($active -and $owner -ne 'DeckBtUsb') { throw 'Bridge child exists without DeckBtUsb owner; no package removed.' }
        if ($active) {
            if (-not (Test-Path -LiteralPath $paramsKey)) { throw 'Live bridge has no Parameters key; no package removed.' }
            # Invalidate old terminal records before removal; only a new driver publication may pass.
            Invoke-RestoreBounded 'Invalidate stale release record' {
                param($Key)
                Set-ItemProperty -LiteralPath $Key -Name UartCompletion -Value 0 -Type DWord
            } @($paramsKey) | Out-Null
        }
        if (Test-Path -LiteralPath $paramsKey) {
            Invoke-RestoreBounded 'Disarm DeckBtUsb' {
                param($Key)
                Set-ItemProperty -LiteralPath $Key -Name Enabled -Value 0 -Type DWord
            } @($paramsKey) | Out-Null
        }
        $bthStart = (Get-CimInstance Win32_Service -Filter "Name='bthserv'").StartMode
        Set-Service bthserv -StartupType Disabled
        Stop-BthservBounded
        if ((Get-Service bthserv).Status -ne 'Stopped') { throw 'bthserv did not stop; nothing was uninstalled.' }
        Invoke-RestorePnp @('/delete-driver', $packages[0].Published, '/uninstall', '/force')
        $remaining = @(Invoke-RestoreBounded 'Confirm package retirement' {
            param($Published)
            Get-DeckStagedPackages | Where-Object { $_.Published -ieq $Published }
        } @($packages[0].Published))
        if ($remaining.Count) { throw 'Project package remains staged; no rescan or enable attempted.' }
        if ($active) {
            $deadline = (Get-Date).AddSeconds(30)
            do {
                $release = Invoke-RestoreBounded 'Read fresh controller handback' {
                    param($Key)
                    if (-not (Test-Path -LiteralPath $Key)) { return $null }
                    $first = Get-ItemProperty -LiteralPath $Key -ErrorAction Stop
                    if ($first.UartCompletion -eq 1 -or $first.UartCompletion -eq 2) {
                        return (Get-ItemProperty -LiteralPath $Key -ErrorAction Stop)
                    }
                    return $first
                } @($paramsKey)
                if ($release.UartCompletion -eq 2) { throw 'Driver retirement unconfirmed (Completion=2); no rescan or enable attempted.' }
                if ($release.UartCompletion -eq 1) { break }
                if ((Get-Date) -ge $deadline) { break }
                Start-Sleep -Milliseconds 500
            } while ((Get-Date) -lt $deadline)
            if ($release.UartCompletion -ne 1 -or $release.UartFailurePhase -ne 'Stopped' -or
                $release.UartHandbackBaud -ne 115200 -or $release.UartHandbackStatus -ne 0) {
                throw "Controller handback unconfirmed (Completion=$($release.UartCompletion), phase=$($release.UartFailurePhase), baud=$($release.UartHandbackBaud), status=$($release.UartHandbackStatus)); no rescan or enable attempted."
            }
            Write-Host '[*] Fresh controller release confirmed: Completion=1, Stopped, 115200 baud, status=0.'
        }
        if (Test-Path -LiteralPath $paramsKey) {
            Invoke-RestoreBounded 'Remove Enabled parameter' {
                param($Key)
                Remove-ItemProperty -LiteralPath $Key -Name Enabled -ErrorAction SilentlyContinue
            } @($paramsKey) | Out-Null
        }
        Set-Service bthserv -StartupType $(if ($bthStart -eq 'Auto') { 'Automatic' } else { $bthStart })
        $bthStart = $null
    } elseif ($owner -ne 'QcBluetooth') {
        throw "Radio owner '$owner' is not QcBluetooth and no project package was found."
    }

    $node = Invoke-RestoreBounded 'Read radio state' {
        param($Id)
        Get-DeckCheckedDevices $Id -PresentOnly
    } @($id)
    if ($node.Problem -in @('CM_PROB_DISABLED', 22)) {
        Invoke-RestorePnp @('/enable-device', $id)
    }
    Invoke-RestorePnp @('/scan-devices')
    $deadline = (Get-Date).AddSeconds(30)
    do {
        try {
            $healthy = Invoke-RestoreBounded 'Verify vendor radio health' { Assert-DeckHealthyVendorRadio }
            if ($healthy.Radio -ieq $id) { break }
        } catch {
            # Only an observed not-yet-healthy radio may be retried, not a failed or timed-out query.
            if ($_.Exception.Message -notlike '*REFUSAL:*' -or (Get-Date) -ge $deadline) { throw }
        }
        if ((Get-Date) -ge $deadline) { throw 'Vendor radio did not become healthy within 30s.' }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    if (-not $healthy -or $healthy.Radio -ine $id) { throw 'Vendor radio not verified within 30s.' }
    if ((Get-Service bthserv).Status -ne 'Running') { Start-Service bthserv }
    Write-Host ("RESTORE VERIFIED: QcBluetooth owns the radio, child healthy; paired devices: {0}" -f @(Get-DeckPairedDevices | Where-Object { $_ }).Count)
    exit 0
} catch {
    Write-Host "RESTORE NOT VERIFIED: $($_.Exception.Message)"
    Write-Host 'STOP: no further device, service, or registry operations in this boot. Save work and preserve this output. If a restart hangs, a hard power-off may be required. After a successful boot, run tools\session.ps1 -Stop once from an elevated console; stop if it fails.'
    exit 1
} finally {
    # Restore configuration only; never start the service after a failed handback.
    if ($null -ne $bthStart) {
        Set-Service bthserv -StartupType $(if ($bthStart -eq 'Auto') { 'Automatic' } else { $bthStart })
    }
}
