<#
    DeckBtUsb synthetic backend installer - install the virtual Bluetooth radio controller and
    observe whether inbox BTHUSB.SYS binds to the emulated USB child on a root devnode.

    WHAT THIS TOUCHES
      - installs the build's test certificate into LocalMachine\Root and \TrustedPublisher
      - enables boot testsigning (needs a reboot; adds the "Test Mode" desktop watermark)
      - stages the DeckBtUsb driver package and creates a root-enumerated devnode

    WHAT THIS DOES NOT TOUCH
      - qcbtuart.sys, the QCA_SHB\UART_H4 radio, or anything else in the vendor Bluetooth stack.
        The emulated radio is an additional, independent device. Existing Bluetooth keeps working
        until the radio slot is yielded for testing.

    The driver is StartType 3 / ErrorControl 1 and root-enumerated. ROOT/STUB treats exact DWORD
    Enabled=1 as a one-shot token and persists DWORD 0 before publishing USB; the running child,
    not the consumed registry token, is activation evidence. Run -Recovery for offline commands.

    USAGE (elevated)
      stub-install.ps1 -Stage Prepare     # certs + testsigning + snapshots, then reboot
      stub-install.ps1 -Stage Install     # after the reboot: stage driver, create devnode, verify
      stub-install.ps1 -Stage Verify      # re-run the checks only
      stub-install.ps1 -Stage Uninstall   # devnode + package + testsigning.
                                          # Not the full restore. The complete rollback
                                          # (all test packages, services, physical radio restore,
                                          # certificate, verification) lives in tools\uninstall.ps1.
      stub-install.ps1 -Recovery          # print the WinRE recovery commands and exit
#>

[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Install', 'Verify', 'Uninstall', 'Solo', 'SoloViaReboot',
                 'Retry', 'Observe', 'Cleanup', 'RestoreRadio',
                 'Arm', 'Disarm', 'PanicRestore')]
    [string] $Stage,
    [switch] $Recovery,
    [switch] $KeepSolo,
    [switch] $DryRun,
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$script:Root       = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$script:Package    = Join-Path $Root "src\driver\x64\$Configuration\deckbtusb"
$script:Inf        = Join-Path $Package 'deckbtusb.inf'
$script:InfName    = 'deckbtusb.inf'
$script:FltPackage = Join-Path $Root "src\filter\x64\$Configuration\deckbtflt"
$script:FltInf     = Join-Path $FltPackage 'deckbtflt.inf'
$script:FltInfName = 'deckbtflt.inf'
$script:HardwareId = 'root\DeckBtUsb'
$script:InstanceId = 'DECKBTUSB'
$script:EmulatedId = 'USB\VID_0CF3&PID_6390'
$script:StateDir   = Join-Path $Root 'recovery'
$script:DevGen     = Join-Path $(if ($env:EWDK) { $env:EWDK } else { 'C:\EWDK' }) 'Program Files\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'
$script:DryRun          = $DryRun
$script:ArmingKey       = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'
$script:ArmingKeyNative = 'HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'

function Write-Step { param($m) Write-Host "[*] $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "[+] $m" -ForegroundColor Green }
function Write-Warn2{ param($m) Write-Host "[!] $m" -ForegroundColor Yellow }
function Write-Err  { param($m) Write-Host "[x] $m" -ForegroundColor Red }

<#
    Native tools (pnputil, bcdedit, reg, devgen) write status text to stderr. Under
    $ErrorActionPreference='Stop' PowerShell promotes that to a terminating NativeCommandError
    and aborts mid-operation - which is exactly how the Track 1 script died between deleting
    registry values and writing the replacement. Every native call goes through here.
#>
function Invoke-Native {
    param([string] $Exe, [string[]] $Arguments, [switch] $Show)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1
        $script:LastNativeOutput = ($out | Out-String)
        $code = $LASTEXITCODE
        if ($Show -and $out) { $out | ForEach-Object { Write-Host "    $_" } }
    } finally {
        $ErrorActionPreference = $prev
    }
    return $code
}

function Show-Recovery {
    @'
If the machine bugchecks or will not boot after installing this driver:

  Windows enters WinRE automatically after two consecutive failed boots.
  WinRE -> Troubleshoot -> Advanced options -> Command Prompt.
  The Windows volume is usually D: in WinRE - confirm with:  dir D:\Windows

  Fastest fix (disable the service; needs no signature checks and no PnP):
      reg load HKLM\OFF D:\Windows\System32\config\SYSTEM
      reg add "HKLM\OFF\ControlSet001\Services\DeckBtUsb" /v Start /t REG_DWORD /d 4 /f
      reg unload HKLM\OFF
      exit  ->  Continue to Windows

  Full removal of the package:
      dism /image:D:\ /get-drivers /format:table
      dism /image:D:\ /remove-driver /driver:oemNN.inf

  Undo test signing:
      bcdedit /store D:\EFI\Microsoft\Boot\BCD /set "{default}" testsigning off
'@ | Write-Host
}

function Assert-Elevated {
    if ($script:DryRun) {
        Write-Step 'DryRun active: skipping administrator elevation check'
        return
    }
    $pr = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
        throw 'Must run elevated. Use the .cmd launcher, which raises UAC.'
    }
}

function Assert-Package {
    if (-not (Test-Path $Inf)) {
        throw "Driver package not found at $Package. Run tools\build.cmd first."
    }
    if (-not (Test-Path $FltInf)) {
        throw "Filter package not found at $FltPackage. Run tools\build.cmd first."
    }
    $sys = Join-Path $Package 'deckbtusb.sys'
    $sig = Get-AuthenticodeSignature $sys
    Write-Ok "Package: $Package"
    Write-Ok "Signature: $($sig.Status) / $($sig.SignerCertificate.Subject)"
    $fsig = Get-AuthenticodeSignature (Join-Path $FltPackage 'deckbtflt.sys')
    Write-Ok "Filter:    $FltPackage"
    Write-Ok "Signature: $($fsig.Status)"
    if ($sig.Status -eq 'UnknownError') {
        Write-Warn2 'UnknownError just means the test cert is not trusted yet - Prepare fixes that'
    }
}

# --------------------------------------------------------------- device discovery

<#
    Find the controller by hardware ID rather than by guessing the instance-ID shape. devgen's
    root devices do not land under a predictable ROOT\SYSTEM\nnnn path, and a wrong filter here
    would make the script believe the devnode is missing and create a duplicate on every run.
#>
function Get-DeckInstanceIds {
    $all = @(Get-PnpDevice -ErrorAction Stop)
    $known = "ROOT\DEVGEN\$InstanceId"
    $ids = @($all | Where-Object { $_.InstanceId -ieq $known } |
             ForEach-Object { $_.InstanceId })

    if ($ids.Count -eq 0) {
        foreach ($d in ($all | Where-Object { $_.InstanceId -like 'ROOT\*' })) {
            try {
                $hw = (Get-PnpDeviceProperty -InstanceId $d.InstanceId `
                         -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction Stop).Data
            } catch {
                # A root device without HardwareIds is not this project's; other failures remain unknown.
                if ($_.FullyQualifiedErrorId -notmatch 'PropertyNotFound|ObjectNotFound') { throw }
                continue
            }
            if ($hw -and (@($hw) -contains $HardwareId)) { $ids += $d.InstanceId }
        }
    }
    return @($ids | Select-Object -Unique)
}

function Get-DeckDevice {
    $ids = Get-DeckInstanceIds
    if ($ids.Count -eq 0) { return @() }
    return @(Get-PnpDevice -InstanceId $ids -ErrorAction Stop)
}

function Get-EmulatedChildren {
    return @(Get-PnpDevice -ErrorAction Stop |
             Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
}

<#
    Every build stamps a fresh DriverVer and `pnputil /add-driver` stages a NEW package each
    time without retiring the old one, so 13 DeckBt packages accumulated across one afternoon
    of iteration. Enumerate them properly and keep only the newest of each INF.
#>
function Get-DeckPackages {
    [void](Invoke-Native pnputil.exe @('/enum-drivers'))
    $blocks = ($script:LastNativeOutput -split "(`r?`n){2,}")
    $pkgs = @()
    foreach ($b in $blocks) {
        $pub = [regex]::Match($b, 'Published Name:\s+(\S+)')
        $org = [regex]::Match($b, 'Original Name:\s+(\S+)')
        $ver = [regex]::Match($b, 'Driver Version:\s+\S+\s+(\S+)')
        if (-not ($pub.Success -and $org.Success)) { continue }
        if ($org.Groups[1].Value -notmatch 'deckbt') { continue }
        $v = $null
        if ($ver.Success) { [void][version]::TryParse($ver.Groups[1].Value, [ref]$v) }
        $pkgs += [pscustomobject]@{
            Published = $pub.Groups[1].Value
            Original  = $org.Groups[1].Value.ToLower()
            Version   = $v
        }
    }
    return $pkgs
}

function Remove-StalePackages {
    $pkgs = Get-DeckPackages
    if ($pkgs.Count -eq 0) { return }

    $removed = 0
    foreach ($group in ($pkgs | Group-Object Original)) {
        # Newest first; everything after the first entry is superseded.
        $ordered = $group.Group | Sort-Object -Property @{ Expression = { $_.Version } } -Descending
        $keep    = $ordered[0]
        Write-Ok "$($group.Name): keeping $($keep.Published) ($($keep.Version))"
        foreach ($old in ($ordered | Select-Object -Skip 1)) {
            $rc = Invoke-Native pnputil.exe @('/delete-driver', $old.Published, '/uninstall', '/force')
            if ($rc -eq 0) { Write-Ok "  removed stale $($old.Published) ($($old.Version))"; $removed++ }
            else { Write-Warn2 "  could not remove $($old.Published) (rc=$rc)" }
        }
    }
    if ($removed -gt 0) { Write-Ok "Pruned $removed stale package(s)" }
    else { Write-Ok 'No stale packages to prune' }
}

<# Resolve the oemNN.inf name the package was published as, by parsing enum-drivers blocks. #>
function Get-DeckPublishedName {
    param([string] $Original = $script:InfName)
    [void](Invoke-Native pnputil.exe @('/enum-drivers'))
    $published = $null
    $names = @()
    foreach ($line in ($script:LastNativeOutput -split "`r?`n")) {
        if ($line -match '^\s*Published Name:\s+(\S+)') { $published = $Matches[1]; continue }
        if ($line -match '^\s*Original Name:\s+(\S+)') {
            if ($published -and $Matches[1] -ieq $Original) { $names += $published }
            $published = $null
        }
    }
    return $names
}

# --------------------------------------------------------------- the real radio

<#
    BTHUSB refuses to start a second adapter: it logs
        "Only one active Bluetooth adapter is supported at a time."   (System log, BTHUSB id 6)
    and fails AddDevice with STATUS_UNSUCCESSFUL. That is why the emulated radio sits at
    CM_PROB_FAILED_ADD while the physical UART radio is running - nothing is wrong with the
    emulated device itself.

    To test the synthetic radio the physical radio must yield the "active adapter" slot. This is a
    plain PnP disable/enable of one devnode: no registry edits, no driver changes, and it is
    reversible. When DeckBtUsb drives the UART controller directly, only one adapter exists.
#>
function Get-RealRadio {
    try {
        return ,@(Get-PnpDevice -PresentOnly -ErrorAction Stop |
                  Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
    } catch {
        throw "Cannot determine physical-radio state safely: $($_.Exception.Message)"
    }
}

function Assert-PhysicalRadioInactive {
    param([string] $Operation)

    Write-Step "Checking the single-radio safety gate before $Operation..."
    try {
        $radio = Get-RealRadio
    } catch {
        Write-Err "REFUSING $Operation`: physical-radio state is unknown."
        throw
    }
    $active = @($radio | Where-Object { $_.Status -eq 'OK' })
    if ($active.Count -gt 0) {
        Write-Err "REFUSING $Operation`: the physical Qualcomm Bluetooth radio is ACTIVE."
        Write-Warn2 "Device: $($active[0].InstanceId) Status: $($active[0].Status)"
        throw "Disable every active physical radio in Device Manager before $Operation."
    }
    Write-Ok 'No active physical Qualcomm radio observed - the adapter slot is free for DeckBtUsb'
}

<#
    pnputil refuses this outright: "Cannot disable critical system device." Neither the radio
    (QCA_SHB\UART_H4, caps 0x80) nor its parent (ACPI\QCOM2066, caps 0x20) advertises
    CM_DEVCAP_REMOVABLE, and pnputil treats any non-removable device as critical.

    Disable-PnpDevice goes through the same SetupDi property-change path Device Manager uses,
    which does not apply that blanket rule. Try the radio, then its parent (disabling the UART
    transport removes the radio child with it). If both refuse, fall back to disabling the
    QcBluetooth service, which needs a reboot but always works.
#>
function Disable-RealRadio {
    $radio = Get-RealRadio
    if ($radio.Count -eq 0) { Write-Warn2 'Real radio not present; nothing to disable'; return $null }

    $radioId  = $radio[0].InstanceId
    $parentId = (Get-PnpDeviceProperty -InstanceId $radioId -KeyName 'DEVPKEY_Device_Parent' `
                   -ErrorAction SilentlyContinue).Data

    foreach ($target in @($radioId, $parentId)) {
        if (-not $target) { continue }
        Write-Step "Disabling $target via the Device Manager API"
        try {
            Disable-PnpDevice -InstanceId $target -Confirm:$false -ErrorAction Stop
        } catch {
            Write-Warn2 "  refused: $($_.Exception.Message -replace '\r?\n', ' ')"
            continue
        }

        Start-Sleep -Seconds 3
        $still = Get-RealRadio
        if ($still.Count -eq 0 -or $still[0].Status -ne 'OK') {
            Write-Ok "Real radio is out of the way (disabled $target)"
            return $target
        }
        Write-Warn2 "  command succeeded but the radio is still OK; trying the next target"
    }

    throw @'
Could not disable the real radio from a script.

Both pnputil and Disable-PnpDevice report "not supported on this OS product": device
enable/disable through those interfaces is fenced off on Windows 11 Home. Device Manager itself
is NOT restricted, so either of these works:

  ROUTE A - Device Manager, no reboot (fastest):
    1. Win+X -> Device Manager
    2. Expand "Bluetooth"
    3. Right-click "Qualcomm Atheros Bluetooth UART Transport Driver"  ->  Disable device
       (if that entry is absent, disable "Bluetooth Radio" instead)
    4. Run  tools\stub-install.ps1 -Stage Retry     <- restarts the emulated radio and reports the result
    5. Re-enable the same device in Device Manager when finished

  ROUTE B - service + reboot (always works):
    1. tools\stub-install.ps1 -Stage SoloViaReboot
    2. reboot
    3. tools\diag.ps1
    4. tools\stub-install.ps1 -Stage RestoreRadio, then reboot again
'@
}

function Enable-RealRadio {
    param([string] $Id)
    if (-not $Id) { $r = Get-RealRadio; if ($r.Count -gt 0) { $Id = $r[0].InstanceId } }
    if (-not $Id) {
        # Disabled devices are not "present"; find it without the PresentOnly filter.
        $r = @(Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
        if ($r.Count -gt 0) { $Id = $r[0].InstanceId }
    }
    if (-not $Id) { throw 'Cannot find the real radio devnode to re-enable.' }

    # Restore the service first, in case the reboot route was used.
    $saved = Join-Path $StateDir 'QcBluetooth-Start.txt'
    if (Test-Path $saved) {
        $orig = (Get-Content $saved -Raw).Trim()
        if ($orig -notmatch '^\d+$') { throw 'Saved QcBluetooth start type is invalid; preserving the recovery file.' }
        $rc = Invoke-Native reg.exe @('add', 'HKLM\SYSTEM\CurrentControlSet\Services\QcBluetooth',
                                      '/v', 'Start', '/t', 'REG_DWORD', '/d', $orig, '/f')
        if ($rc -ne 0) { throw "Could not restore QcBluetooth start type (rc=$rc)." }
        Write-Ok "Restored QcBluetooth service Start=$orig (reboot if the radio does not start)"
        Remove-Item $saved -Force -ErrorAction Stop
    }

    Write-Step "Re-enabling the real radio $Id"
    try {
        Enable-PnpDevice -InstanceId $Id -Confirm:$false -ErrorAction Stop
        Write-Ok 'Enable-PnpDevice succeeded'
    } catch {
        Write-Warn2 "Enable-PnpDevice: $($_.Exception.Message -replace '\r?\n', ' ')"
        $rc = Invoke-Native pnputil.exe @('/enable-device', $Id)
        if ($rc -ne 0) { throw "Radio enable failed or needs restart (rc=$rc)." }
    }
    # The parent may be the one that was disabled.
    $parentId = (Get-PnpDeviceProperty -InstanceId $Id -KeyName 'DEVPKEY_Device_Parent' `
                   -ErrorAction SilentlyContinue).Data
    if ($parentId) {
        $parent = Get-PnpDevice -InstanceId $parentId -ErrorAction Stop
        if ("$($parent.Problem)" -in @('22', 'CM_PROB_DISABLED')) {
            Enable-PnpDevice -InstanceId $parentId -Confirm:$false -ErrorAction Stop
        }
    }
    Start-Sleep -Seconds 3

    $now = @(Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
    if ($now.Count -gt 0 -and $now[0].Status -eq 'OK') {
        Write-Ok 'Real Bluetooth radio is back and OK'
    } else {
        throw 'Real radio is not healthy. Save the output, restart Windows, then verify the stock radio before continuing.'
    }
}

function Restart-Emulated {
    $child = Get-EmulatedChildren
    foreach ($c in $child) {
        Write-Step "Restarting emulated radio $($c.InstanceId)"
        [void](Invoke-Native pnputil.exe @('/restart-device', $c.InstanceId))
    }
    if ($child.Count -eq 0) {
        Write-Warn2 'Emulated radio devnode not present; rescanning'
        [void](Invoke-Native pnputil.exe @('/scan-devices'))
    }
    Start-Sleep -Seconds 4
}
function Set-DeckBtUsbArmingGate {
    param([int] $Enabled)
    $val = if ($Enabled -eq 1) { 1 } else { 0 }
    $desc = if ($val -eq 1) { '1 (Armed: UDE virtual radio creation permitted)' } else { '0 (Disarmed: driver stays idle)' }
    Write-Step "Setting arming gate Enabled=$desc"
    if ($script:DryRun) {
        Write-Host "    [DRY-RUN] reg.exe add `"$ArmingKeyNative`" /v Enabled /t REG_DWORD /d $val /f" -ForegroundColor Yellow
    } else {
        $rc = Invoke-Native reg.exe @('add', $ArmingKeyNative, '/v', 'Enabled', '/t', 'REG_DWORD', '/d', "$val", '/f')
        if ($rc -ne 0) { throw "Failed to set arming gate Enabled=$val (reg.exe rc=$rc)" }
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters')
        try {
            if ($null -eq $key -or
                $key.GetValueKind('Enabled') -ne [Microsoft.Win32.RegistryValueKind]::DWord -or
                [int]$key.GetValue('Enabled', -1) -ne $val) {
                throw "Arming gate read-back did not confirm exact REG_DWORD $val"
            }
        } finally {
            if ($null -ne $key) { $key.Dispose() }
        }
        Write-Ok "Arming gate verified: Enabled=$val (REG_DWORD)"
    }
}

function Get-DeckBtUsbArmingGate {
    if (-not (Test-Path $ArmingKey)) { return $null }
    $p = Get-ItemProperty -Path $ArmingKey -ErrorAction SilentlyContinue
    return $p.Enabled
}

function Restart-DeckDevnode {
    Write-Step "Restarting root devnode ROOT\DEVGEN\$InstanceId"
    if ($script:DryRun) {
        Write-Host "    [DRY-RUN] pnputil.exe /restart-device `"ROOT\DEVGEN\$InstanceId`"" -ForegroundColor Yellow
        Write-Host "    [DRY-RUN] pnputil.exe /scan-devices" -ForegroundColor Yellow
    } else {
        $rc = Invoke-Native pnputil.exe @('/restart-device', "ROOT\DEVGEN\$InstanceId")
        if ($rc -ne 0) {
            throw "Failed to restart ROOT\DEVGEN\$InstanceId (pnputil rc=$rc)"
        }
        Start-Sleep -Seconds 4
    }
}

function Remove-DeckDevnodeSurgical {
    $devs = Get-DeckDevice
    if ($devs.Count -eq 0) {
        Write-Ok "Virtual devnode ROOT\DEVGEN\$InstanceId is not present (nothing to remove)"
        if ($script:DryRun) {
            Write-Host "    [DRY-RUN] (If devnode were present: pnputil.exe /remove-device `"ROOT\DEVGEN\$InstanceId`" /subtree)" -ForegroundColor Yellow
        }
        return
    }
    foreach ($d in $devs) {
        Write-Step "Surgically removing devnode $($d.InstanceId) /subtree"
        if ($script:DryRun) {
            Write-Host "    [DRY-RUN] pnputil.exe /remove-device `"$($d.InstanceId)`" /subtree" -ForegroundColor Yellow
        } else {
            $rc = Invoke-Native pnputil.exe @('/remove-device', $d.InstanceId, '/subtree')
            if ($rc -ne 0) { throw "Failed to remove $($d.InstanceId) (pnputil rc=$rc)" }
            Write-Ok "Removed $($d.InstanceId)"
        }
    }
    if (-not $script:DryRun -and (Get-DeckDevice).Count -ne 0) {
        throw 'Surgical removal did not eliminate every owned DeckBtUsb root devnode'
    }
}

function Repair-RadioManagementState {
    Write-Step 'Checking and repairing Windows Radio Management state'

    # 1. RadioState in QCA_SHB\UART_H4\*\Device Parameters
    # When Code 31 occurs, Windows Radio Management writes RadioState = 2 (Software Disabled).
    # Setting it back to 1 restores Software On state.
    $qcaBase = 'HKLM:\SYSTEM\CurrentControlSet\Enum\QCA_SHB\UART_H4'
    if (Test-Path $qcaBase) {
        $subkeys = Get-ChildItem $qcaBase -ErrorAction SilentlyContinue
        foreach ($sk in $subkeys) {
            $devParamPath = Join-Path $sk.PSPath 'Device Parameters'
            $nativeDevParam = "HKLM\SYSTEM\CurrentControlSet\Enum\QCA_SHB\UART_H4\$($sk.PSChildName)\Device Parameters"
            if (Test-Path $devParamPath) {
                $cur = (Get-ItemProperty -Path $devParamPath -ErrorAction SilentlyContinue).RadioState
                if ($null -ne $cur) {
                    Write-Step "Found RadioState=$cur on $nativeDevParam"
                    if ($cur -ne 1) {
                        Write-Step 'Repairing RadioState=1 (On)'
                        if ($script:DryRun) {
                            Write-Host "    [DRY-RUN] reg.exe add `"$nativeDevParam`" /v RadioState /t REG_DWORD /d 1 /f" -ForegroundColor Yellow
                        } else {
                            $rc = Invoke-Native reg.exe @('add', $nativeDevParam, '/v', 'RadioState', '/t', 'REG_DWORD', '/d', '1', '/f')
                            if ($rc -ne 0) { throw "Failed to repair RadioState on $nativeDevParam (reg.exe rc=$rc)" }
                            Write-Ok "Repaired RadioState = 1 on $nativeDevParam"
                        }
                    } else {
                        Write-Ok "RadioState is already 1 (On) on $nativeDevParam"
                    }
                } else {
                    Write-Ok "RadioState value absent on $nativeDevParam (clean)"
                }
            }
        }
    } else {
        Write-Ok 'QCA_SHB\UART_H4 key absent; no RadioState to repair'
    }

    # 2. BluetoothRadioState in Control\RadioManagement\{afd198ac-5f30-4e89-a789-5ddf60a69366}\BluetoothRadioState
    # When Code 31 occurs, Windows sets BluetoothRadioState = 0. Setting it to 1 restores the radio state.
    $rmKey = 'HKLM:\System\CurrentControlSet\Control\RadioManagement\{afd198ac-5f30-4e89-a789-5ddf60a69366}\BluetoothRadioState'
    $rmNative = 'HKLM\System\CurrentControlSet\Control\RadioManagement\{afd198ac-5f30-4e89-a789-5ddf60a69366}\BluetoothRadioState'
    if (Test-Path $rmKey) {
        $rmProp = Get-ItemProperty -Path $rmKey -ErrorAction SilentlyContinue
        $rmVal = $rmProp.'(default)'
        if ($null -eq $rmVal) {
            Write-Ok 'BluetoothRadioState default value absent; no observed On/Off state'
        } elseif ($rmVal -ne 1) {
            Write-Step "Found BluetoothRadioState (Default)=$rmVal"
            Write-Step 'Repairing BluetoothRadioState = 1'
            if ($script:DryRun) {
                Write-Host "    [DRY-RUN] reg.exe add `"$rmNative`" /ve /t REG_DWORD /d 1 /f" -ForegroundColor Yellow
            } else {
                $rc = Invoke-Native reg.exe @('add', $rmNative, '/ve', '/t', 'REG_DWORD', '/d', '1', '/f')
                if ($rc -ne 0) { throw "Failed to repair BluetoothRadioState (reg.exe rc=$rc)" }
                Write-Ok 'Repaired BluetoothRadioState = 1'
            }
        } else {
            Write-Ok 'BluetoothRadioState is already 1 (On)'
        }
    } else {
        Write-Ok 'BluetoothRadioState key absent; no radio lockout recorded'
    }

    # 3. BluetoothUserService & bthserv
    Write-Step 'Ensuring Bluetooth services are running'
    if ($script:DryRun) {
        Write-Host '    [DRY-RUN] net.exe start bthserv' -ForegroundColor Yellow
        Write-Host '    [DRY-RUN] Start-Service BluetoothUserService*' -ForegroundColor Yellow
    } else {
        $bth = Get-Service bthserv -ErrorAction Stop
        if ($bth.Status -ne 'Running') {
            Write-Step 'Starting bthserv...'
            Start-Service bthserv -ErrorAction Stop
        }
        $userSvcs = @(Get-Service BluetoothUserService* -ErrorAction Stop)
        foreach ($s in $userSvcs) {
            if ($s.Status -ne 'Running') {
                Write-Step "Starting $($s.Name)..."
                Start-Service $s.Name -ErrorAction Stop
            }
        }
        $bth = Get-Service bthserv -ErrorAction Stop
        $stoppedUsers = @(Get-Service BluetoothUserService* -ErrorAction Stop |
                          Where-Object { $_.Status -ne 'Running' })
        if ($bth.Status -ne 'Running' -or $stoppedUsers.Count -gt 0) {
            throw 'Bluetooth service postcondition failed after repair'
        }
        Write-Ok 'Bluetooth services observed running'
    }
}

<#
    Virtual radio verification: does Windows treat this as a usable adapter? A started
    devnode is necessary but not sufficient - BTHPORT must also build the Bluetooth stack on top
    of it (the enumerators that carry pairing, RFCOMM and LE).
#>
function Show-BluetoothStack {
    Write-Host ''
    Write-Host '--- Bluetooth stack built on top of the radio ---'
    $want = @{
        'BTH\MS_BTHBRB' = 'Microsoft Bluetooth Enumerator (classic; pairing + profiles)'
        'BTH\MS_BTHLE'  = 'Microsoft Bluetooth LE Enumerator'
        'BTH\MS_RFCOMM' = 'RFCOMM Protocol TDI'
    }
    $found = 0
    foreach ($id in $want.Keys) {
        $d = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like "$id*" })
        if ($d.Count -gt 0) {
            Write-Ok "$($d[0].Status.ToString().PadRight(3)) $($want[$id])"
            $found++
        } else {
            Write-Warn2 "absent: $($want[$id])"
        }
    }
    if ($found -gt 0) {
        Write-Ok "CRITERION 3: Windows built its Bluetooth stack on the emulated radio ($found/3 enumerators)"
    } else {
        Write-Warn2 'CRITERION 3: no Bluetooth enumerators - the radio started but the stack did not come up'
    }

    $radioState = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters' `
                                   -ErrorAction SilentlyContinue
    $svc = Get-Service bthserv -ErrorAction SilentlyContinue
    Write-Host "        bthserv: $($svc.Status)"
}

function Show-BthUsbEvents {
    Write-Host ''
    Write-Host '--- recent BTHUSB / BTHPORT event log entries ---'
    $ev = Get-WinEvent -LogName System -MaxEvents 400 -ErrorAction SilentlyContinue |
          Where-Object { $_.TimeCreated -gt (Get-Date).AddMinutes(-6) -and $_.ProviderName -match 'BTH' }
    if (-not $ev) { Write-Host '  (none in the last 6 minutes)'; return }
    foreach ($e in ($ev | Select-Object -First 8)) {
        Write-Host ("  [{0}] {1} id={2} {3}: {4}" -f $e.TimeCreated.ToString('HH:mm:ss'),
                    $e.ProviderName, $e.Id, $e.LevelDisplayName,
                    ($e.Message -replace '\r?\n', ' '))
    }
}

# --------------------------------------------------------------- certificate

function Install-TestCert {
    $catalog = Join-Path $Package 'deckbtusb.cat'
    $cert = (Get-AuthenticodeSignature -LiteralPath $catalog).SignerCertificate
    if (-not $cert) { throw 'The package catalog has no signer certificate. Run tools\build.cmd first.' }

    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    $cer = Join-Path $StateDir 'DeckBtUsbTestCert.cer'
    if (Test-Path -LiteralPath $cer) {
        $saved = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
        try {
            if ($saved.Thumbprint -ne $cert.Thumbprint) {
                throw 'Package signer differs from the saved project certificate; preserving existing recovery evidence.'
            }
        } finally { $saved.Dispose() }
    } else {
        Export-Certificate -Cert $cert -FilePath $cer -ErrorAction Stop | Out-Null
    }

    # Root makes the chain trusted; TrustedPublisher suppresses the PnP install prompt.
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root            | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Ok "Trusted $($cert.Subject)"
    Write-Ok "Thumbprint $($cert.Thumbprint)"
}

function Remove-TestCert {
    $cer = Join-Path $StateDir 'DeckBtUsbTestCert.cer'
    if (-not (Test-Path -LiteralPath $cer)) {
        throw 'Saved project certificate unavailable; refusing automatic certificate removal.'
    }
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
    try {
        foreach ($store in 'Root', 'TrustedPublisher') {
            $path = "Cert:\LocalMachine\$store\$($cert.Thumbprint)"
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Force -ErrorAction Stop
                if (Test-Path -LiteralPath $path) { throw "Certificate remains in $store." }
                Write-Ok "Removed $($cert.Thumbprint) from LocalMachine\$store"
            }
        }
    } finally { $cert.Dispose() }
}

# --------------------------------------------------------------- pre-flight state

function Save-Snapshots {
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    $backup = Join-Path $StateDir 'bcd-backup.bcd'
    if (-not (Test-Path -LiteralPath $backup)) {
        $rc = Invoke-Native bcdedit.exe @('/export', $backup)
        if ($rc -ne 0 -or -not (Test-Path -LiteralPath $backup)) { throw "BCD export failed (rc=$rc)." }
    }
    foreach ($snapshot in @(
        @{ Name = 'drivers-before.txt'; Arguments = @('/enum-drivers') },
        @{ Name = 'bt-before.txt'; Arguments = @('/enum-devices', '/class', 'Bluetooth', '/drivers', '/stack') }
    )) {
        $path = Join-Path $StateDir $snapshot.Name
        if (-not (Test-Path -LiteralPath $path)) {
            $rc = Invoke-Native pnputil.exe $snapshot.Arguments
            if ($rc -ne 0) { throw "Snapshot $($snapshot.Name) failed (rc=$rc)." }
            $script:LastNativeOutput | Set-Content -LiteralPath $path -ErrorAction Stop
        }
    }

    Write-Ok "Snapshots and BCD backup in $StateDir"

    try {
        Enable-ComputerRestore -Drive 'C:\' -ErrorAction Stop
        Checkpoint-Computer -Description "pre-DeckBtUsb-$(Get-Date -f yyyyMMdd-HHmm)" `
                            -RestorePointType MODIFY_SETTINGS -ErrorAction Stop
        Write-Ok 'System restore point created'
    } catch {
        Write-Warn2 "Restore point not created: $($_.Exception.Message)"
    }
}

function Set-BootFlags {
    [void](Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'on'))
    [void](Invoke-Native bcdedit.exe @('/set', '{current}', 'nocrashautoreboot', 'on'))
    [void](Invoke-Native bcdedit.exe @('/set', '{current}', 'bootstatuspolicy', 'DisplayAllFailures'))

    [void](Invoke-Native bcdedit.exe @('/enum', '{current}'))
    if ($script:LastNativeOutput -match 'testsigning\s+Yes') {
        Write-Ok 'testsigning ON (verified), nocrashautoreboot ON, bootstatuspolicy DisplayAllFailures'
    } else {
        throw 'testsigning did not stick - check Secure Boot state before continuing'
    }
}

function Clear-BootFlags {
    [void](Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'off'))
    [void](Invoke-Native bcdedit.exe @('/deletevalue', '{current}', 'nocrashautoreboot'))
    # /deletevalue, not /set IgnoreAllFailures: stock Windows omits this element, and
    # IgnoreAllFailures suppresses Startup Repair rather than restoring pre-project behavior.
    [void](Invoke-Native bcdedit.exe @('/deletevalue', '{current}', 'bootstatuspolicy'))
    Write-Ok 'testsigning OFF, nocrashautoreboot and bootstatuspolicy deleted'
}

# --------------------------------------------------------------- install / remove

function Install-Package {
    # ORDER IS LOAD-BEARING. `/add-driver /install` binds the package to devices that exist AT
    # THAT MOMENT; creating the devnode afterwards does not re-trigger a match, and setupapi
    # logs "Unable to find any matching devices" while leaving a healthy but driverless node.
    # So: create the device first, then stage+install, then force a rescan.

    <#
        Stage BOTH packages before any device work. Staging needs no device, and the filter INF
        must already be in the driver store when the emulated child first enumerates - otherwise
        PnP binds bth.inf on the compatible ID and the DeckBtFlt lower filter never gets installed.
        deckbtflt.inf matches the HARDWARE id USB\VID_0CF3&PID_6390, which outranks bth.inf's
        compatible-id match, and it defers to bth.inf via Include/Needs so BTHUSB still runs.
    #>
    if ($script:DryRun) {
        Write-Host "    [DRY-RUN] pnputil.exe /add-driver `"$FltInf`"" -ForegroundColor Yellow
        if ((Get-DeckDevice).Count -gt 0) {
            Write-Host "    [DRY-RUN] Would reuse ROOT\DEVGEN\$InstanceId" -ForegroundColor Yellow
        } else {
            Write-Host "    [DRY-RUN] `"$DevGen`" /add /bus ROOT /instanceid $InstanceId /hardwareid $HardwareId" -ForegroundColor Yellow
        }
        Write-Host "    [DRY-RUN] pnputil.exe /add-driver `"$Inf`" /install" -ForegroundColor Yellow
        Write-Host '    [DRY-RUN] pnputil.exe /scan-devices' -ForegroundColor Yellow
        Write-Host '    [DRY-RUN] Would inspect binding state and recreate only an unbound or failed DeckBtUsb root devnode' -ForegroundColor Yellow
        Write-Host '    [DRY-RUN] Would prune only superseded DeckBtUsb packages' -ForegroundColor Yellow
        return
    }
    Write-Step 'Staging deckbtflt.inf (BTHUSB + bus-interface filter for the emulated radio)'
    $rc = Invoke-Native pnputil.exe @('/add-driver', $FltInf) -Show
    if ($rc -ne 0 -and $rc -ne 259 -and $rc -ne 3010) { throw "staging deckbtflt.inf failed with $rc" }

    if ((Get-DeckDevice).Count -gt 0) {
        Write-Ok 'Root devnode already present; reusing it'
    } else {
        Write-Step "Creating root-enumerated devnode $HardwareId (instance $InstanceId)"
        if (Test-Path $DevGen) {
            $rc = Invoke-Native $DevGen @('/add', '/bus', 'ROOT', '/instanceid', $InstanceId,
                                          '/hardwareid', $HardwareId) -Show
            if ($rc -ne 0) { Write-Warn2 "devgen returned $rc" }
        } else {
            Write-Warn2 "devgen.exe not found at $DevGen"
            Write-Warn2 'Create it manually: hdwwiz.exe -> install hardware manually -> Show All'
            Write-Warn2 "-> Have Disk -> $Inf"
            return
        }
    }

    Write-Step "Staging $InfName and binding it to the devnode"
    $rc = Invoke-Native pnputil.exe @('/add-driver', $Inf, '/install') -Show
    if ($rc -ne 0 -and $rc -ne 259 -and $rc -ne 3010) { throw "pnputil /add-driver failed with $rc" }
    if ($script:LastNativeOutput -match 'Unable to find any matching devices') {
        Write-Warn2 'pnputil reported no matching devices - forcing a device rescan'
    }

    $names = Get-DeckPublishedName
    if ($names.Count -gt 0) { Write-Ok "Published as $($names -join ', ')" }

    Write-Step 'Rescanning for hardware changes'
    [void](Invoke-Native pnputil.exe @('/scan-devices'))
    Start-Sleep -Seconds 3

    # If the emulated child already exists bound to plain bth.inf, bind the better-ranked DeckBtFlt INF.
    $child = Get-EmulatedChildren
    if ($child.Count -gt 0) {
        Write-Step 'Binding deckbtflt.inf to the emulated radio (adds the lower filter)'
        $rc = Invoke-Native pnputil.exe @('/add-driver', $FltInf, '/install') -Show
        if ($rc -ne 0 -and $rc -ne 259 -and $rc -ne 3010) { Write-Warn2 "filter bind returned $rc" }
        foreach ($c in $child) {
            [void](Invoke-Native pnputil.exe @('/restart-device', $c.InstanceId))
        }
        Start-Sleep -Seconds 3
    }

    Write-Step 'Pruning superseded driver packages'
    Remove-StalePackages

    <#
        Recreate the devnode when it is unbound OR sitting in an error state. A node that
        already failed AddDevice keeps its failure until PnP retries; simply staging a newer
        package does not re-run AddDevice against it. Removing and re-adding forces a clean
        attempt with whatever driver version is now published - which is exactly what is needed
        after rebuilding the driver.
    #>
    foreach ($d in (Get-DeckDevice)) {
        $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId `
                  -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        if ($svc -and $d.Status -eq 'OK') { continue }

        if (-not $svc) { Write-Warn2 "$($d.InstanceId) has no driver bound; recreating it" }
        else { Write-Warn2 "$($d.InstanceId) is in state '$($d.Status)'; recreating it to force a fresh AddDevice" }

        [void](Invoke-Native pnputil.exe @('/remove-device', $d.InstanceId, '/subtree'))
        Start-Sleep -Seconds 2
        [void](Invoke-Native $DevGen @('/add', '/bus', 'ROOT', '/instanceid', $InstanceId,
                                       '/hardwareid', $HardwareId) -Show)
        Start-Sleep -Seconds 2
        [void](Invoke-Native pnputil.exe @('/scan-devices'))
        Start-Sleep -Seconds 3
    }
}

function Uninstall-Package {
    foreach ($d in Get-DeckDevice) {
        Write-Step "Removing devnode $($d.InstanceId)"
        $rc = Invoke-Native pnputil.exe @('/remove-device', $d.InstanceId, '/subtree')
        if ($rc -ne 0) { throw "Device removal failed or needs restart (rc=$rc)." }
    }

    $names = @(Get-DeckPublishedName $InfName) + @(Get-DeckPublishedName $FltInfName)
    if ($names.Count -eq 0) { Write-Warn2 'No published DeckBtUsb package found in the driver store' }
    foreach ($n in $names) {
        Write-Step "Deleting driver store package $n"
        $rc = Invoke-Native pnputil.exe @('/delete-driver', $n, '/uninstall', '/force') -Show
        if ($rc -ne 0) { throw "Package $n removal failed or needs restart (rc=$rc)." }
    }

    $left = @(Get-DeckPublishedName $InfName) + @(Get-DeckPublishedName $FltInfName)
    if ($left.Count -gt 0) { throw "Packages remain staged: $($left -join ', ')." }
    if (@(Get-DeckDevice).Count -gt 0) { throw 'Project devnode remains present after removal.' }
    Write-Ok 'Project package and devnode removal verified'
}

# --------------------------------------------------------------- acceptance

function Test-StubAcceptance {
    Write-Host ''
    Write-Host '===== Synthetic backend acceptance =====' -ForegroundColor Cyan

    $ctrl = Get-DeckDevice
    if ($ctrl.Count -gt 0) {
        foreach ($c in $ctrl) {
            Write-Host "  controller: $($c.Status)  $($c.FriendlyName)  ::  $($c.InstanceId)"
            if ($c.Status -ne 'OK') {
                $p = (Get-PnpDeviceProperty -InstanceId $c.InstanceId `
                        -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
                $s = (Get-PnpDeviceProperty -InstanceId $c.InstanceId `
                        -KeyName 'DEVPKEY_Device_ProblemStatus' -ErrorAction SilentlyContinue).Data
                Write-Err "  controller problem=$p ntstatus=$s"
            }
        }
        $csvc = (Get-PnpDeviceProperty -InstanceId $ctrl[0].InstanceId `
                   -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $cinf = (Get-PnpDeviceProperty -InstanceId $ctrl[0].InstanceId `
                   -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction SilentlyContinue).Data
        Write-Host "  bound service: '$csvc'   inf: '$cinf'"
        if (-not $csvc) {
            Write-Err 'CONTROLLER HAS NO DRIVER BOUND - the devnode exists but deckbtusb.sys never loaded'
        } elseif ($ctrl[0].Status -eq 'OK') {
            Write-Ok "Controller started, driven by '$csvc'"
        }
    } else {
        Write-Err 'Controller devnode not present'
    }

    Write-Host ''
    Write-Host '--- emulated USB child ---'
    [void](Invoke-Native pnputil.exe @('/enum-devices', '/deviceid', $EmulatedId, '/drivers', '/stack') -Show)

    $child = Get-EmulatedChildren
    if ($child.Count -eq 0) {
        Write-Err 'CRITERION 1 FAILED: no USB\VID_0CF3&PID_6390 devnode - the device never enumerated'
    } elseif ($child[0].Status -eq 'OK') {
        Write-Ok 'CRITERION 1 PASSED: emulated USB Bluetooth radio enumerated and STARTED'
        $svc = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                  -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        $lf = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                 -KeyName 'DEVPKEY_Device_LowerFilters' -ErrorAction SilentlyContinue).Data
        Write-Host "  bound service: $svc   lower filters: $($lf -join ',')"
        if ($svc -ieq 'BTHUSB') { Write-Ok 'CRITERION 2 PASSED: inbox BTHUSB.SYS owns the device' }
        else { Write-Warn2 "CRITERION 2: expected BTHUSB, got '$svc'" }
    } else {
        Write-Err "CRITERION 1 PARTIAL: devnode exists but status is $($child[0].Status)"
        $lf = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                 -KeyName 'DEVPKEY_Device_LowerFilters' -ErrorAction SilentlyContinue).Data
        $svc = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                 -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        Write-Host "  bound service: $svc   lower filters: $($lf -join ',')"
        $p = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        $s = (Get-PnpDeviceProperty -InstanceId $child[0].InstanceId `
                -KeyName 'DEVPKEY_Device_ProblemStatus' -ErrorAction SilentlyContinue).Data
        Write-Err "  problem=$p ntstatus=$s"
    }

    Write-Host ''
    Write-Host '--- real radio ---'
    $radio = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
    if ($radio.Count -gt 0 -and $radio[0].Status -eq 'OK') {
        Write-Ok "Real Bluetooth radio OK ($($radio[0].InstanceId))"
    } elseif ($Stage -eq 'Solo' -or $Stage -eq 'Retry' -or $Stage -eq 'Observe') {
        Write-Warn2 'Real radio is disabled - deliberate for this test.'
        Write-Warn2 'Re-enable it in Device Manager: System devices -> Qualcomm Atheros Bluetooth UART Transport Driver'
    } else {
        Write-Err 'Real Bluetooth radio is NOT healthy - run -Stage Uninstall'
    }

    Write-Host ''
    Write-Host '--- devices in a problem state ---'
    [void](Invoke-Native pnputil.exe @('/enum-devices', '/problem'))
    foreach ($line in ($script:LastNativeOutput -split "`r?`n")) {
        if ($line -match 'Instance ID:|Problem') { Write-Host "  $($line.Trim())" }
    }

    Write-Host ''
    Get-Service bthserv, BTAGService, BthAvctpSvc -ErrorAction SilentlyContinue |
        Select-Object Name, Status | Format-Table -AutoSize | Out-String | Write-Host
}

# --------------------------------------------------------------- main

if ($Recovery) { Show-Recovery; exit 0 }
if (-not $Stage) { throw 'Specify -Stage Prepare|Install|Verify|Uninstall, or -Recovery.' }

Assert-Elevated

switch ($Stage) {
    'Prepare' {
        Assert-Package
        Save-Snapshots
        Install-TestCert
        Set-BootFlags
        Write-Host ''
        Write-Ok 'Prepared. Reboot now, then run tools\stub-install.ps1 -Stage Install'
    }
    'Install' {
        Assert-Package
        [void](Invoke-Native bcdedit.exe @('/enum', '{current}'))
        if ($script:LastNativeOutput -notmatch 'testsigning\s+Yes') {
            throw 'testsigning is not active. Run tools\stub-install.ps1 -Stage Prepare and reboot first.'
        }
        Write-Ok 'testsigning is active'
        Assert-PhysicalRadioInactive -Operation 'INSTALL'
        Set-DeckBtUsbArmingGate -Enabled 0
        Install-Package
        if ($script:DryRun) {
            Write-Host '===== RESULT: DRY-RUN PLAN COMPLETE (No package or devnode was changed) =====' -ForegroundColor Green
        } else {
            Write-Ok 'Package and root devnode are ready with the virtual radio DISARMED.'
            Write-Host 'Next: run tools\stub-install.ps1 -Stage Arm while the physical radio remains disabled.'
        }
    }
    'Verify'    { Test-StubAcceptance; Show-BluetoothStack; Show-BthUsbEvents }
    'Observe' {
        <#
            Observe-only: start the emulated radio exactly ONCE and report. No staging, no
            rebinding, no rescan - so the EP0 trace contains a single clean initialisation
            sequence instead of one per install step, which is what made the earlier traces
            look like a retry loop.
        #>
        Assert-PhysicalRadioInactive -Operation 'OBSERVE'
        if ((Get-DeckDevice).Count -eq 0) {
            throw 'Root devnode is absent. Run tools\stub-install.ps1 -Stage Retry, then tools\stub-install.ps1 -Stage Arm before Observe.'
        }
        Write-Step 'Clearing the trace by restarting the controller once'
        foreach ($d in (Get-DeckDevice)) {
            if ($script:DryRun) {
                Write-Host "    [DRY-RUN] pnputil.exe /restart-device `"$($d.InstanceId)`"" -ForegroundColor Yellow
            } else {
                [void](Invoke-Native pnputil.exe @('/restart-device', $d.InstanceId))
            }
        }
        if (-not $script:DryRun) { Start-Sleep -Seconds 6 }
        Test-StubAcceptance
        Show-BluetoothStack
        Show-BthUsbEvents
        if ($script:DryRun) {
            Write-Host '===== RESULT: DRY-RUN PLAN COMPLETE (No restart or observation transition occurred) =====' -ForegroundColor Green
        }
    }
    'Solo' {
        Assert-Package
        Write-Host ''
        Write-Warn2 'Bluetooth will be OFF for about 30 seconds while the real radio steps aside.'
        Write-Host ''
        $realId = $null
        try {
            $realId = Disable-RealRadio
            Restart-Emulated
            Test-StubAcceptance
            Show-BthUsbEvents
        } finally {
            if (-not $KeepSolo) {
                Write-Host ''
                Enable-RealRadio -Id $realId
            } else {
                Write-Host ''
                Write-Warn2 'KeepSolo: the real radio is STILL DISABLED.'
                Write-Warn2 'Run tools\stub-install.ps1 -Stage RestoreRadio when you are done looking.'
            }
        }
    }
    'SoloViaReboot' {
        Assert-Package
        New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
        $svcKey = 'HKLM\SYSTEM\CurrentControlSet\Services\QcBluetooth'
        $cur = (Get-ItemProperty -LiteralPath ($svcKey -replace '^HKLM\\','HKLM:\') `
                  -ErrorAction SilentlyContinue).Start
        if ($null -eq $cur) { throw "Cannot read $svcKey\Start" }
        Set-Content -Path (Join-Path $StateDir 'QcBluetooth-Start.txt') -Value "$cur"
        Write-Ok "Saved original QcBluetooth Start=$cur to recovery\QcBluetooth-Start.txt"

        if ((Invoke-Native reg.exe @('add', $svcKey, '/v', 'Start', '/t', 'REG_DWORD', '/d', '4', '/f')) -ne 0) {
            throw 'Failed to disable the QcBluetooth service'
        }
        Write-Ok 'QcBluetooth service set to Disabled (Start=4)'
        Write-Host ''
        Write-Warn2 'REBOOT NOW. Bluetooth will be absent after the reboot - that is the point:'
        Write-Warn2 'the emulated radio becomes the only adapter, so BTHUSB can start it.'
        Write-Host ''
        Write-Host 'After the reboot:  tools\diag.ps1    (read the result)'
        Write-Host 'To undo:           tools\stub-install.ps1 -Stage RestoreRadio, then reboot again'
    }
    'Retry' {
        <#
            Assumes the real radio has already been disabled by hand in Device Manager.
            The gate is deliberately disarmed before package or devnode work. Retry recreates the
            root node without publishing a Bluetooth personality; Arm supplies the separate,
            guarded one-shot token.
        #>
        Assert-Package
        Assert-PhysicalRadioInactive -Operation 'RETRY'
        Set-DeckBtUsbArmingGate -Enabled 0
        Install-Package
        if ($script:DryRun) {
            Write-Host '===== RESULT: DRY-RUN PLAN COMPLETE (No package or devnode was changed) =====' -ForegroundColor Green
        } else {
            Write-Ok 'Current package and root devnode are ready with the virtual radio DISARMED.'
            Write-Host 'Next: run tools\stub-install.ps1 -Stage Arm while the physical radio remains disabled.'
        }
    }
    'Cleanup' {
        Write-Step 'Driver store before'
        foreach ($p in (Get-DeckPackages)) { Write-Host "    $($p.Published)  $($p.Original)  $($p.Version)" }
        Write-Host ''
        Remove-StalePackages
        Write-Host ''
        Write-Step 'Driver store after'
        foreach ($p in (Get-DeckPackages)) { Write-Host "    $($p.Published)  $($p.Original)  $($p.Version)" }
        Write-Host ''
        Write-Host 'To remove DeckBtUsb entirely instead, run tools\stub-install.ps1 -Stage Uninstall'
    }
    'RestoreRadio' { Enable-RealRadio -Id $null }
    'Uninstall' {
        Uninstall-Package
        Remove-TestCert
        Clear-BootFlags
        Write-Ok 'Uninstalled. Reboot to drop test signing.'
    }
    'Arm' {
        Write-Host ''
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host '  DeckBtUsb: ARM Emulated Radio' -ForegroundColor Cyan
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host 'WHAT THIS DOES:'
        Write-Host '  1. Verifies the physical Qualcomm Bluetooth radio is NOT active.'
        Write-Host '  2. Requires the root devnode to exist (Retry recreates it after PanicRestore).'
        Write-Host '  3. Sets Enabled = 1 in HKLM\...\Services\DeckBtUsb\Parameters.'
        Write-Host '  4. Restarts the existing root devnode to create the virtual USB radio.'
        Write-Host '  5. Verifies whether inbox BTHUSB bound to the emulated radio child.'
        Write-Host ''
        Write-Host 'TO REVERSE:'
        Write-Host '  Run tools\stub-install.ps1 -Stage Disarm to disable the emulated radio cleanly.'
        Write-Host ''

        # Windows permits only ONE active Bluetooth adapter. Every activation route uses this
        # guard and there is no override.
        Assert-PhysicalRadioInactive -Operation 'ARM'

        # PanicRestore removes this node. Arming is not node creation: Retry must recreate it
        # while disarmed before this route may set Enabled=1.
        if ((Get-DeckDevice).Count -eq 0) {
            Write-Err "REFUSING TO ARM: root devnode ROOT\DEVGEN\$InstanceId is absent."
            throw 'Run tools\stub-install.ps1 -Stage Retry first, then re-run tools\stub-install.ps1 -Stage Arm.'
        }

        # Treat setting the one-shot token and restarting as one operation. A failed attempt
        # must explicitly return the token to Enabled=0.
        try {
            Set-DeckBtUsbArmingGate -Enabled 1
            Restart-DeckDevnode
        } catch {
            $armFailure = $_
            Write-Err "Arm mutation failed: $($armFailure.Exception.Message)"
            try {
                Set-DeckBtUsbArmingGate -Enabled 0
                Restart-DeckDevnode
            } catch {
                Write-Err "CRITICAL: rollback to Enabled=0 failed: $($_.Exception.Message)"
            }
            throw $armFailure
        }

        # Step 4: Report device state
        Write-Host ''
        Write-Host '--- Device State Verification ---' -ForegroundColor Cyan
        $devs = Get-DeckDevice
        if ($devs.Count -gt 0) {
            foreach ($d in $devs) {
                $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
                Write-Ok "Devnode $($d.InstanceId): Status=$($d.Status) Service='$svc'"
            }
        } else {
            Write-Err "Root devnode ROOT\DEVGEN\$InstanceId disappeared during Arm. Run tools\stub-install.ps1 -Stage Retry before retrying."
        }

        $child = Get-EmulatedChildren
        $healthyChildren = @($child | Where-Object {
            if ($_.Status -ne 'OK') { return $false }
            $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data
            return ($svc -ieq 'BTHUSB')
        })
        if ($script:DryRun) {
            Write-Host '    [DRY-RUN] Would verify emulated child USB\VID_0CF3&PID_6390 status and BTHUSB binding' -ForegroundColor Yellow
            Write-Host ''
            Write-Host '===== RESULT: DRY RUN (No live changes made) =====' -ForegroundColor Green
        } elseif ($healthyChildren.Count -gt 0) {
            Write-Ok "Emulated radio child is present, OK, and owned by BTHUSB ($($healthyChildren[0].InstanceId))"
            Show-BluetoothStack
            Write-Host ''
            Write-Host '===== RESULT: PASS (One-shot token consumed; emulated radio observed active) =====' -ForegroundColor Green
            Write-Host 'Disarm before restoring the physical radio: tools\stub-install.ps1 -Stage Disarm'
        } else {
            Write-Err 'Emulated radio child not started or not OK; rolling the gate back to Enabled=0.'
            if ($child.Count -gt 0) {
                Write-Err "Child status: $($child[0].Status) ($($child[0].InstanceId))"
            }
            Set-DeckBtUsbArmingGate -Enabled 0
            Restart-DeckDevnode
            Write-Host 'Run tools\diag.ps1 to inspect driver breadcrumbs.' -ForegroundColor Yellow
            Write-Host ''
            Write-Host '===== RESULT: FAIL (Gate returned to DISARMED) =====' -ForegroundColor Red
            throw 'Arm failed; Enabled was returned to 0.'
        }
    }
    'Disarm' {
        Write-Host ''
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host '  DeckBtUsb: DISARM Emulated Radio' -ForegroundColor Cyan
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host 'WHAT THIS DOES:'
        Write-Host '  1. Sets Enabled = 0 in HKLM\...\Services\DeckBtUsb\Parameters.'
        Write-Host '  2. Restarts the root devnode so the virtual USB radio is cleanly removed.'
        Write-Host '  3. Leaves staged drivers, test certificates, and testsigning 100% intact.'
        Write-Host '  4. Reminds you to re-enable the physical Qualcomm radio in Device Manager.'
        Write-Host ''
        Write-Host 'TO RE-ARM:'
        Write-Host '  Run tools\stub-install.ps1 -Stage Arm (after disabling physical radio in Device Manager).'
        Write-Host ''

        # Step 1: Set arming gate Enabled = 0
        Set-DeckBtUsbArmingGate -Enabled 0

        # Step 2: Restart devnode so PrepareHardware runs with Enabled=0 and does not create UDE device
        Restart-DeckDevnode

        # Step 3: Verify emulated child is gone
        $child = Get-EmulatedChildren
        if ($script:DryRun) {
            Write-Host '    [DRY-RUN] Would verify emulated child USB\VID_0CF3&PID_6390 is absent' -ForegroundColor Yellow
        } elseif ($child.Count -eq 0) {
            Write-Ok 'Emulated USB radio child is cleanly removed'
        } else {
            throw "Disarm failed: emulated child remains present ($($child[0].InstanceId), Status=$($child[0].Status))"
        }

        if (-not $script:DryRun) {
            foreach ($d in (Get-DeckDevice)) {
                Write-Ok "Root devnode $($d.InstanceId) is idle (Status=$($d.Status))"
            }
        }

        Write-Host ''
        Write-Host 'NEXT STEP TO RESTORE NORMAL BLUETOOTH:' -ForegroundColor Cyan
        Write-Host '  In Device Manager: expand "Bluetooth" -> right-click'
        Write-Host '  "Qualcomm Atheros Bluetooth UART Transport Driver" -> Enable device.'
        Write-Host ''
        if ($script:DryRun) {
            Write-Host '===== RESULT: DRY-RUN PLAN COMPLETE (No radio was disarmed) =====' -ForegroundColor Green
        } else {
            Write-Host '===== RESULT: PASS (Emulated radio disarmed) =====' -ForegroundColor Green
        }
        Write-Host 'To re-arm later: run tools\stub-install.ps1 -Stage Arm'
    }
    'PanicRestore' {
        Write-Host ''
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host '  DeckBtUsb: EMERGENCY PANIC RESTORE BLUETOOTH' -ForegroundColor Cyan
        Write-Host '=====================================================' -ForegroundColor Cyan
        Write-Host 'WHAT THIS DOES (Surgical Rollback):'
        Write-Host '  1. Removes ONLY the virtual devnode ROOT\DEVGEN\DECKBTUSB /subtree.'
        Write-Host '  2. Disarms the driver gate (Enabled = 0) so it cannot accidentally re-arm.'
        Write-Host '  3. Repairs Radio Management state corrupted by Code 31:'
        Write-Host '     - RadioState = 1 (On) on Qualcomm UART device parameters'
        Write-Host '     - BluetoothRadioState = 1 (On) in RadioManagement registry'
        Write-Host '     - Starts BluetoothUserService and bthserv'
        Write-Host '  4. Verifies physical Qualcomm radio health.'
        Write-Host '  5. Leaves staged drivers, test certificates, and testsigning 100% INTACT.'
        Write-Host '     (No two-reboot penalty, no driver store re-staging required).'
        Write-Host ''

        # Step 1: Surgically remove virtual devnode
        Remove-DeckDevnodeSurgical

        # Step 2: Ensure arming gate is disarmed
        Set-DeckBtUsbArmingGate -Enabled 0

        # Step 3: Repair Radio Management registry & services
        Repair-RadioManagementState

        # Step 4: Verify physical radio health
        Write-Host ''
        Write-Host '--- Physical Radio Verification ---' -ForegroundColor Cyan
        $radio = Get-RealRadio
        $healthy = ($radio.Count -gt 0 -and $radio[0].Status -eq 'OK')

        if ($script:DryRun) {
            Write-Host "Observed physical-radio baseline: $(if ($healthy) { 'present and healthy' } else { 'not healthy/unknown' })"
            Write-Host ''
            Write-Host '===== RESULT: DRY-RUN PLAN COMPLETE (No Bluetooth state was restored) =====' -ForegroundColor Green
        } elseif ($healthy) {
            Write-Ok "Physical Bluetooth radio is PRESENT and HEALTHY ($($radio[0].InstanceId))"
            Write-Host ''
            Write-Host '===== RESULT: PASS (Bluetooth restored surgically) =====' -ForegroundColor Green
        } else {
            # Check if it exists disabled
            $allRadio = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
            if ($allRadio.Count -gt 0) {
                Write-Warn2 "Physical radio found but status is: $($allRadio[0].Status)"
            } else {
                Write-Warn2 'Physical radio devnode not currently present.'
            }

            Write-Host ''
            Write-Host 'MANUAL STEPS REQUIRED IF BLUETOOTH STILL NOT WORKING:' -ForegroundColor Yellow
            Write-Host '  Why manual? Windows 11 Home blocks scripted device enable/disable'
            Write-Host '  (pnputil and Disable-PnpDevice report "not supported on this OS product",'
            Write-Host '   and pnputil refuses because the radio is a critical system device).'
            Write-Host ''
            Write-Host '  1. Re-enable in Device Manager:'
            Write-Host '     Win+X -> Device Manager -> Bluetooth'
            Write-Host '     Right-click "Qualcomm Atheros Bluetooth UART Transport Driver" -> Enable device'
            Write-Host ''
            Write-Host '  2. If the UI toggle in Settings is off:'
            Write-Host '     Settings -> Bluetooth & devices -> Toggle Bluetooth to ON'
            Write-Host '     (Or toggle Airplane Mode ON, wait 3 seconds, toggle Airplane Mode OFF)'
            Write-Host ''
            Write-Host '  3. If disabled via SoloViaReboot (service Start=4):'
            Write-Host '     Run tools\stub-install.ps1 -Stage RestoreRadio and reboot.'
            Write-Host ''
            Write-Host '===== RESULT: ATTENTION NEEDED (Follow manual steps above) =====' -ForegroundColor Yellow
        }

        Write-Host ''
        Write-Host 'Staged drivers, test certs, and testsigning remain intact.'
        Write-Host 'To resume DeckBtUsb testing later:'
        Write-Host '  1. Disable the physical radio in Device Manager'
        Write-Host '  2. Run tools\stub-install.ps1 -Stage Retry to recreate the disarmed root devnode'
        Write-Host '  3. Run tools\stub-install.ps1 -Stage Arm to arm and restart that existing devnode'
        if (-not $script:DryRun -and -not $healthy) {
            throw 'PanicRestore completed its surgical actions, but physical Bluetooth health was not observed'
        }
    }
}
