<#
    session.ps1 - operator session runner.

      -Prepare       Export BCD backup, export catalog test certificate if absent, capture baseline
                     if absent, set testsigning on, and disable the vendor radio. Reboot afterwards.
      -Identify      Trust test certificate, verify radio disabled, and run uart-identify.ps1.
      -Probe         Trust test certificate, verify radio disabled, and run uart-probe.ps1.
      -Bridge        Run uart-probe.ps1 in bridge mode (optional -HoldSeconds <n>).
      -Start         Run uart-probe.ps1 in bridge mode with -KeepBound to leave driver bound.
      -Stop          Run restore-radio.ps1 to restore radio to vendor driver.
      -Uninstall     Run uninstall.ps1 to remove packages, services, certificates, and test signing.

    Output: tools\_build\sessions\<stamp>\
#>
[CmdletBinding()]
param(
    [switch] $Prepare,
    [switch] $Identify,
    [switch] $Probe,
    [switch] $Bridge,
    [ValidateRange(0, 900)] [int] $HoldSeconds = 0,
    [switch] $Start,
    [switch] $Stop,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')
if (-not (Test-DeckElevated)) { Write-Host 'REFUSAL: run elevated.'; exit 2 }

$root = Split-Path -Parent $PSScriptRoot
$ps = Join-Path $PSHOME 'powershell.exe'
$session = Join-Path $PSScriptRoot ("_build\sessions\{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $session | Out-Null
Start-Transcript -LiteralPath (Join-Path $session 'session.transcript.log') | Out-Null
$exitCode = 1

function Invoke-Tool([string] $Script, [string[]] $Arguments) {
    # Pipe through the host so the session transcript records the child's output.
    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $Script) @Arguments 2>&1 |
        ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}
function Invoke-Native([string] $Exe, [string[]] $Arguments, [int] $TimeoutSec = 60) {
    $psi = [Diagnostics.ProcessStartInfo]::new($Exe, ($Arguments -join ' '))
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { throw "TIMEOUT: $Exe $($Arguments -join ' ') exceeded ${TimeoutSec}s." }
    $p.WaitForExit()
    $text = ($out.Result + $err.Result).Trim()
    Write-Host $text
    return [pscustomobject]@{ Code = $p.ExitCode; Output = $text }
}

# Returns the vendor-owned radio node; refuses anything else.
function Get-VendorRadio {
    $nodes = @(Get-DeckCheckedDevices 'ACPI\QCOM2066*' -PresentOnly)
    if ($nodes.Count -ne 1 -or $nodes[0].Service -ne $VendorService) {
        throw 'REFUSAL: expected exactly one ACPI\QCOM2066 node owned by QcBluetooth.'
    }
    return $nodes[0]
}

# Stops bthserv, bounded. A plain Stop-Service can wait indefinitely if bthserv sits in STOP_PENDING.
# Its svchost hosts bthserv alone, so ending that process is the service stopping;
# a shared host is never touched.
function Stop-BthservBounded([int] $TimeoutSeconds = 20) {
    if ((Get-Service bthserv).Status -eq 'Stopped') { return }
    Stop-Service bthserv -Force -NoWait
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline -and (Get-Service bthserv).Status -ne 'Stopped') { Start-Sleep -Milliseconds 500 }
    if ((Get-Service bthserv).Status -eq 'Stopped') { return }
    $svc = Get-CimInstance Win32_Service -Filter "Name='bthserv'"
    $shared = @(Get-CimInstance Win32_Service | Where-Object { $_.ProcessId -eq $svc.ProcessId -and $_.Name -ne 'bthserv' })
    if ($svc.ProcessId -and $shared.Count -eq 0) {
        Write-Host "[*] bthserv still $($svc.State) after ${TimeoutSeconds}s; ending its dedicated host process $($svc.ProcessId)."
        Stop-Process -Id $svc.ProcessId -Force
        $deadline = (Get-Date).AddSeconds(10)
        while ((Get-Date) -lt $deadline -and (Get-Service bthserv).Status -ne 'Stopped') { Start-Sleep -Milliseconds 500 }
    }
    if ((Get-Service bthserv).Status -ne 'Stopped') { throw 'bthserv did not stop; the radio was not touched.' }
}

# Disables the vendor radio. Returns 'disabled' or 'pending' (applies at the next boot).
# bthserv is trigger-started by the radio it serves: a plain stop is undone within seconds and the
# restarted service vetoes the disable (Kernel-PnP 225).
# Its start type is therefore Disabled for the duration and restored afterwards.
function Disable-VendorRadio([string] $Radio) {
    $bthWasRunning = (Get-Service bthserv).Status -eq 'Running'
    $bthStart = (Get-CimInstance Win32_Service -Filter "Name='bthserv'").StartMode
    try {
        Set-Service bthserv -StartupType Disabled
        Stop-BthservBounded
        $r = Invoke-Native 'pnputil.exe' @('/disable-device', "`"$Radio`"")
        if ($r.Code -eq 3010 -or $r.Output -match '(?i)reboot') { return 'pending' }
        if ($r.Code -ne 0) { throw "pnputil /disable-device exit $($r.Code)." }
        return 'disabled'
    } finally {
        Set-Service bthserv -StartupType $(if ($bthStart -eq 'Auto') { 'Automatic' } else { $bthStart })
        if ($bthWasRunning -and (Get-Service bthserv).Status -ne 'Running') { Start-Service bthserv }
    }
}

try {
    $modeCount = @($Prepare, $Identify, $Probe, $Bridge, $Start, $Stop, $Uninstall | Where-Object { $_.IsPresent }).Count
    if ($modeCount -ne 1) {
        throw 'REFUSAL: specify exactly one mode: -Prepare, -Identify, -Probe, -Bridge [-HoldSeconds n], -Start, -Stop, or -Uninstall.'
    }
    if ($HoldSeconds -gt 0 -and -not $Bridge) {
        throw 'REFUSAL: -HoldSeconds is only valid with -Bridge.'
    }

    if ($Prepare) {
        $certFile = Join-Path $root 'recovery\DeckBtUsbTestCert.cer'
        if (-not (Test-Path -LiteralPath $certFile)) {
            $catPath = Join-Path $root 'src\driver\x64\Release\deckbtusb\deckbtusb.cat'
            if (-not (Test-Path -LiteralPath $catPath)) {
                throw "REFUSAL: package catalog missing at '$catPath'. Build the driver package (tools\build.cmd) before running -Prepare."
            }
            $sig = Get-AuthenticodeSignature -LiteralPath $catPath
            if (-not $sig.SignerCertificate) {
                throw "REFUSAL: package catalog at '$catPath' is not signed. Build and sign the driver package before running -Prepare."
            }
            $certDir = Split-Path -Parent $certFile
            if (-not (Test-Path -LiteralPath $certDir)) { New-Item -ItemType Directory -Force -Path $certDir | Out-Null }
            [IO.File]::WriteAllBytes($certFile, $sig.SignerCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
            Write-Host "[+] exported signer certificate to $certFile ($($sig.SignerCertificate.Thumbprint))"
        }
        # Pre-project snapshots used by uninstall.ps1 and verify-clean.ps1 (written once).
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DeckDriversBefore) | Out-Null
        if (-not (Test-Path -LiteralPath $DeckDriversBefore)) {
            $snapshot = & pnputil.exe /enum-drivers | Out-String
            if ($LASTEXITCODE -ne 0) { throw 'pnputil /enum-drivers failed; nothing changed.' }
            Set-Content -LiteralPath $DeckDriversBefore -Value $snapshot
        }
        if (-not (Test-Path -LiteralPath $DeckBtBefore)) {
            $snapshot = & pnputil.exe /enum-devices /class Bluetooth /drivers /stack | Out-String
            if ($LASTEXITCODE -ne 0) { throw 'pnputil /enum-devices failed; nothing changed.' }
            Set-Content -LiteralPath $DeckBtBefore -Value $snapshot
        }
        $backup = Join-Path $root 'recovery\bcd-backup.bcd'
        if (-not (Test-Path -LiteralPath $backup)) {
            $r = Invoke-Native 'bcdedit.exe' @('/export', "`"$backup`"")
            if ($r.Code -ne 0) { throw 'BCD export failed; nothing changed.' }
            Write-Host "[+] exported BCD backup to $backup"
        } else {
            $timeBackup = Join-Path $root ("recovery\bcd-pre-session-{0}.bcd" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
            $r = Invoke-Native 'bcdedit.exe' @('/export', "`"$timeBackup`"")
            if ($r.Code -ne 0) { throw 'BCD export failed; nothing changed.' }
        }
        if (-not (Test-Path -LiteralPath $DeckBaselineJson)) {
            Write-Host '[*] baseline absent; capturing baseline while vendor radio is healthy...'
            $baseExit = Invoke-Tool 'capture-baseline.ps1' @()
            if ($baseExit -ne 0 -or -not (Test-Path -LiteralPath $DeckBaselineJson)) {
                throw "REFUSAL: capture-baseline.ps1 failed (exit $baseExit); baseline required before taking the radio."
            }
        }
        $r = Invoke-Native 'bcdedit.exe' @('/set', '{current}', 'testsigning', 'on')
        if ($r.Code -ne 0) { throw 'bcdedit /set testsigning on failed.' }
        $flags = Get-DeckBootFlags
        if (-not $flags.Readable -or $flags.TestSigning -ne 'on') { throw 'testsigning readback is not on.' }
        if (@($flags.Leftovers).Count) { throw "debug boot flags present: $($flags.Leftovers -join ', ')" }
        $node = Get-VendorRadio
        $svcKey = Join-Path $DeckServicesRegRoot 'DeckBtUsb'
        if (Test-Path -LiteralPath $svcKey) {
            if (@(Get-DeckStagedPackages | Where-Object { $_.IsOurs }).Count) { throw 'REFUSAL: a project package is still staged; run tools\session.ps1 -Uninstall first.' }
            [void](Invoke-Native 'sc.exe' @('stop', 'DeckBtUsb'))
            [void](Invoke-Native 'sc.exe' @('delete', 'DeckBtUsb'))
            if (Test-Path -LiteralPath $svcKey) { Remove-Item -LiteralPath $svcKey -Recurse -Force -ErrorAction SilentlyContinue }
            if (Test-Path -LiteralPath $svcKey) { throw 'Leftover DeckBtUsb service could not be removed; reboot, then rerun -Prepare.' }
            Write-Host '[*] leftover DeckBtUsb service and Parameters removed.'
        }
        $disableState = if ("$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')) { 'disabled' } else { Disable-VendorRadio $node.InstanceId }
        Write-Host "PREPARE PASS: testsigning on, no debug boot flags, radio disable $disableState."
        Write-Host 'Restart Windows now unless test signing was already active in this boot. Bluetooth stays off until tools\session.ps1 -Start (DeckBtUsb) or -Stop (stock driver). Test signing stays on until tools\session.ps1 -Uninstall and another restart.'
        $exitCode = 0
    }

    if ($Identify -or $Probe -or $Bridge -or $Start) {
        if ((Invoke-Tool 'trust-cert.ps1' @()) -ne 0) {
            $exitCode = 2
            throw 'Test signing not active or certificate trust not verified; no device was touched.'
        }
        $node = Get-VendorRadio
        $disabled = "$($node.Problem)" -in @('22', 'CM_PROB_DISABLED')
        Write-Host "[*] radio $($node.InstanceId) vendor-owned; disabled=$disabled; paired devices: $(@(Get-DeckPairedDevices).Count)"
        if (-not $disabled) {
            if ($node.Status -eq 'OK' -and @(Get-DeckCheckedDevices 'QCA_SHB\UART_H4*' -PresentOnly).Count -eq 0) {
                Write-Host '[*] vendor radio has no Bluetooth child (controller stranded); taking it over.'
            } else {
                Assert-DeckHealthyVendorRadio
            }
            $connected = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object { $_.InstanceId -like 'BTHLE\DEV_*' -or $_.InstanceId -like 'BTHENUM\DEV_*' } |
                Where-Object {
                    (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 15' -ErrorAction SilentlyContinue).Data -eq $true
                })
            if ($connected.Count) {
                $exitCode = 2
                throw ('REFUSAL: Bluetooth device(s) connected: ' + (($connected | ForEach-Object { $_.FriendlyName }) -join ', ') +
                    '. Switch them off (or disconnect them), then rerun; nothing was changed.')
            }
            if ((Disable-VendorRadio $node.InstanceId) -eq 'pending') {
                throw 'Radio disable was vetoed and is now pending: reboot, then rerun tools\session.ps1 (the radio will boot disabled).'
            }
        }
        if ($Start) {
            $log = Join-Path $session 'bridge.log'
            $exitCode = Invoke-Tool 'uart-probe.ps1' @('-Force', '-Bridge', '-KeepBound', '-LogPath', $log)
            Write-Host "[*] uart-probe (bridge keepbound) exit $exitCode (log $log)"
        } elseif ($Bridge) {
            $log = Join-Path $session 'bridge.log'
            $bridgeArgs = @('-Force', '-Bridge', '-LogPath', $log)
            if ($HoldSeconds -gt 0) { $bridgeArgs += @('-HoldSeconds', "$HoldSeconds") }
            $exitCode = Invoke-Tool 'uart-probe.ps1' $bridgeArgs
            Write-Host "[*] uart-probe (bridge) exit $exitCode (log $log)"
        } elseif ($Probe) {
            $log = Join-Path $session 'probe.log'
            $exitCode = Invoke-Tool 'uart-probe.ps1' @('-Force', '-LogPath', $log)
            Write-Host "[*] uart-probe exit $exitCode (log $log)"
        } else {
            $log = Join-Path $session 'identify.log'
            $exitCode = Invoke-Tool 'uart-identify.ps1' @('-Force', '-LogPath', $log)
            Write-Host "[*] uart-identify exit $exitCode (log $log)"
        }
        if ($exitCode -eq 0) {
            if ((Get-Service bthserv).Status -ne 'Running') { Start-Service bthserv -ErrorAction SilentlyContinue }
            Write-Host "[*] paired devices after: $(@(Get-DeckPairedDevices | Where-Object { $_ }).Count)"
        }
    }

    if ($Stop) {
        $exitCode = Invoke-Tool 'restore-radio.ps1' @()
        Write-Host "[*] restore-radio exit $exitCode"
    }

    if ($Uninstall) {
        if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '_build\vendorlog\armed-original-params.json')) {
            [void](Invoke-Tool 'vendor-log.ps1' @('-Disarm', '-RemoveLog'))
        }
        $exitCode = Invoke-Tool 'uninstall.ps1' @('-Force')
        Write-Host "[*] uninstall exit $exitCode. Reboot before gaming; then tools\verify-clean.ps1 (elevated) must report CLEAN."
    }
} catch {
    Write-Host "[-] SESSION FAIL: $($_.Exception.Message)"
    if ($exitCode -eq 0) { $exitCode = 1 }
} finally {
    Write-Host "SESSION RESULT exit=$exitCode dir=$session"
    Stop-Transcript | Out-Null
}
exit $exitCode
