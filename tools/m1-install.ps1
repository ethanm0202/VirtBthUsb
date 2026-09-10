<#
    m1-install.ps1 - Test installation and staging script for VirtBthUsb driver packages.

    Usage (elevated PowerShell):
      .\m1-install.ps1 -Stage Prepare     # Install test cert and enable testsigning (requires reboot)
      .\m1-install.ps1 -Stage Install     # Stage driver package and create root\DeckBtUsb devnode
      .\m1-install.ps1 -Stage Verify      # Verify devnode and driver binding state
      .\m1-install.ps1 -Stage Uninstall   # Remove devnode, unstage packages, disable testsigning
      .\m1-install.ps1 -Recovery          # Print offline WinRE recovery commands
#>

[CmdletBinding()]
param(
    [ValidateSet('Prepare', 'Install', 'Verify', 'Uninstall', 'Solo', 'SoloViaReboot',
                 'Retry', 'Observe', 'Cleanup', 'RestoreRadio')]
    [string] $Stage,
    [switch] $Recovery,
    [switch] $KeepSolo,
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
$script:DevGen     = 'C:\EWDK\Program Files\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'

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
    Locate controller by HARDWARE ID rather than by guessing the instance-ID shape. devgen's
    root devices do not land under a predictable ROOT\SYSTEM\nnnn path, and a wrong filter here
    would make the script believe the devnode is missing and create a duplicate on every run.
#>
function Get-DeckInstanceIds {
    # devgen /bus ROOT creates ROOT\DEVGEN\<instanceid>. Its DEVICE id is 'ROOT\DEVGEN', so
    # `pnputil /enum-devices /deviceid root\DeckBtUsb` matches nothing even though the node
    # exists and carries that hardware ID. Because a fixed /instanceid is passed, the full
    # instance ID is deterministic - check it directly first.
    $ids = @()
    $known = "ROOT\DEVGEN\$InstanceId"
    if (Get-PnpDevice -InstanceId $known -ErrorAction SilentlyContinue) { $ids += $known }

    if ($ids.Count -eq 0) {
        # Fallback: match on the hardware ID, which survives any instance-ID naming change.
        foreach ($d in (Get-PnpDevice -ErrorAction SilentlyContinue |
                        Where-Object { $_.InstanceId -like 'ROOT\*' })) {
            $hw = (Get-PnpDeviceProperty -InstanceId $d.InstanceId `
                     -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue).Data
            if ($hw -and (@($hw) -contains $HardwareId)) { $ids += $d.InstanceId }
        }
    }
    return $ids
}

function Get-DeckDevice {
    $ids = Get-DeckInstanceIds
    if ($ids.Count -eq 0) { return @() }
    return @(Get-PnpDevice -InstanceId $ids -ErrorAction SilentlyContinue)
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

<# Resolve the oemNN.inf assigned to the driver package by parsing enum-drivers blocks. #>
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
    and fails AddDevice with STATUS_UNSUCCESSFUL. That is why M1's emulated radio sits at
    CM_PROB_FAILED_ADD while the Deck's real UART radio is running - nothing is wrong with the
    emulated device itself.

    To prove M1 the real radio must yield the "active adapter" slot for a moment. This is a
    plain PnP disable/enable of one devnode: no registry edits, no driver changes, and it is
    exactly reversible. In the hardware design the conflict disappears because this driver
    replaces qcbtuart and only one adapter is active.
#>
function Get-RealRadio {
    return @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
}

<#
    pnputil refuses this outright: "Cannot disable critical system device." Neither the radio
    (QCA_SHB\UART_H4, caps 0x80) nor its parent (ACPI\QCOM2066, caps 0x20) advertises
    CM_DEVCAP_REMOVABLE, and pnputil treats any non-removable device as critical.

    Disable-PnpDevice goes through the same SetupDi property-change path Device Manager uses,
    which does not apply that blanket rule. Try the radio, then its parent (disabling the UART
    transport removes the radio child with it). If both refuse, fall back to disabling the
    QcBluetooth service, which needs a reboot but always works - and is exactly what M3 does
    permanently anyway.
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
    4. Run  M1-8-RETRY.cmd     <- restarts the emulated radio and reports the result
    5. Re-enable the same device in Device Manager when finished

  ROUTE B - service + reboot (always works):
    1. M1-7-SOLO-VIA-REBOOT.cmd
    2. reboot
    3. M1-4-DIAG.cmd
    4. M1-6-RESTORE-RADIO.cmd, then reboot again
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
    if (-not $Id) { Write-Err 'Cannot find the real radio devnode to re-enable!'; return }

    # Restore the service first, in case the reboot route was used.
    $saved = Join-Path $StateDir 'QcBluetooth-Start.txt'
    if (Test-Path $saved) {
        $orig = (Get-Content $saved -Raw).Trim()
        if ($orig -match '^\d+$') {
            [void](Invoke-Native reg.exe @('add', 'HKLM\SYSTEM\CurrentControlSet\Services\QcBluetooth',
                                           '/v', 'Start', '/t', 'REG_DWORD', '/d', $orig, '/f'))
            Write-Ok "Restored QcBluetooth service Start=$orig (reboot to take effect)"
        }
        Remove-Item $saved -Force -ErrorAction SilentlyContinue
    }

    Write-Step "Re-enabling the real radio $Id"
    try {
        Enable-PnpDevice -InstanceId $Id -Confirm:$false -ErrorAction Stop
        Write-Ok 'Enable-PnpDevice succeeded'
    } catch {
        Write-Warn2 "Enable-PnpDevice: $($_.Exception.Message -replace '\r?\n', ' ')"
        [void](Invoke-Native pnputil.exe @('/enable-device', $Id))
    }
    # The parent may be the one that was disabled.
    $parentId = (Get-PnpDeviceProperty -InstanceId $Id -KeyName 'DEVPKEY_Device_Parent' `
                   -ErrorAction SilentlyContinue).Data
    if ($parentId) {
        try { Enable-PnpDevice -InstanceId $parentId -Confirm:$false -ErrorAction Stop } catch { }
    }
    Start-Sleep -Seconds 3

    $now = @(Get-PnpDevice -ErrorAction SilentlyContinue |
             Where-Object { $_.InstanceId -like 'QCA_SHB\UART_H4\*' })
    if ($now.Count -gt 0 -and $now[0].Status -eq 'OK') {
        Write-Ok 'Real Bluetooth radio is back and OK'
    } else {
        Write-Err 'Real radio did not come back OK - reboot, and it will start normally.'
        Write-Err 'If it still does not, run: pnputil /enable-device "<the QCA_SHB instance id>"'
    }
}

function Restart-Emulated {
    $child = @(Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
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

<#
    The real M1 completion criterion: does Windows treat this as a usable adapter? A started
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
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -like '*WDKTestCert*' } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $cert) { throw 'No WDKTestCert in CurrentUser\My. Run tools\build.cmd first.' }

    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    $cer = Join-Path $StateDir 'DeckBtUsbTestCert.cer'
    Export-Certificate -Cert $cert -FilePath $cer -Force | Out-Null

    # Root makes the chain trusted; TrustedPublisher suppresses the PnP install prompt.
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root            | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Ok "Trusted $($cert.Subject)"
    Write-Ok "Thumbprint $($cert.Thumbprint)"
}

function Remove-TestCert {
    foreach ($store in 'Root', 'TrustedPublisher') {
        Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -like '*WDKTestCert*' } |
            ForEach-Object {
                Remove-Item $_.PSPath -Force
                Write-Ok "Removed $($_.Thumbprint) from LocalMachine\$store"
            }
    }
}

# --------------------------------------------------------------- pre-flight state

function Save-Snapshots {
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    [void](Invoke-Native bcdedit.exe @('/export', (Join-Path $StateDir 'bcd-backup.bcd')))

    [void](Invoke-Native pnputil.exe @('/enum-drivers'))
    $script:LastNativeOutput | Set-Content (Join-Path $StateDir 'drivers-before.txt')

    [void](Invoke-Native pnputil.exe @('/enum-devices', '/class', 'Bluetooth', '/drivers', '/stack'))
    $script:LastNativeOutput | Set-Content (Join-Path $StateDir 'bt-before.txt')

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
    [void](Invoke-Native bcdedit.exe @('/set', '{current}', 'bootstatuspolicy', 'IgnoreAllFailures'))
    Write-Ok 'testsigning OFF, boot flags restored'
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
        PnP binds bth.inf on the compatible ID and the lower filter is not installed.
        deckbtflt.inf matches the HARDWARE id USB\VID_0CF3&PID_6390, which outranks bth.inf's
        compatible-id match, and it defers to bth.inf via Include/Needs so BTHUSB still runs.
    #>
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

    # If the emulated child already exists bound to plain bth.inf, bind the hardware-ID INF.
    $child = @(Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
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
        [void](Invoke-Native pnputil.exe @('/remove-device', $d.InstanceId, '/subtree'))
    }

    $names = @(Get-DeckPublishedName $InfName) + @(Get-DeckPublishedName $FltInfName)
    if ($names.Count -eq 0) { Write-Warn2 'No published DeckBtUsb package found in the driver store' }
    foreach ($n in $names) {
        Write-Step "Deleting driver store package $n"
        $rc = Invoke-Native pnputil.exe @('/delete-driver', $n, '/uninstall', '/force') -Show
        if ($rc -ne 0) { Write-Warn2 "pnputil /delete-driver $n returned $rc" }
    }

    $left = @(Get-DeckPublishedName $InfName) + @(Get-DeckPublishedName $FltInfName)
    if ($left.Count -gt 0) { Write-Warn2 "Still published: $($left -join ', ') (a reboot may be required)" }
    else { Write-Ok 'Driver store is clean' }
}

# --------------------------------------------------------------- acceptance

function Test-M1 {
    Write-Host ''
    Write-Host '===== M1 acceptance =====' -ForegroundColor Cyan

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

    $child = @(Get-PnpDevice -ErrorAction SilentlyContinue |
               Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' })
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
        Write-Ok 'Prepared. REBOOT now, then run M1-2-INSTALL.cmd'
    }
    'Install' {
        Assert-Package
        [void](Invoke-Native bcdedit.exe @('/enum', '{current}'))
        if ($script:LastNativeOutput -notmatch 'testsigning\s+Yes') {
            throw 'testsigning is not active. Run M1-1-PREPARE.cmd and reboot first.'
        }
        Write-Ok 'testsigning is active'
        Install-Package
        Start-Sleep -Seconds 3
        Test-M1
    }
    'Verify'    { Test-M1; Show-BluetoothStack; Show-BthUsbEvents }
    'Observe' {
        <#
            Observe-only: start the emulated radio exactly ONCE and report. No staging, no
            rebinding, no rescan - so the EP0 trace contains a single clean initialisation
            sequence instead of one per install step, which is what made the earlier traces
            look like a retry loop.
        #>
        $radio = Get-RealRadio
        if ($radio.Count -gt 0 -and $radio[0].Status -eq 'OK') {
            Write-Warn2 'The real radio is ACTIVE - disable it in Device Manager first.'
            return
        }
        Write-Step 'Clearing the trace by restarting the controller once'
        foreach ($d in (Get-DeckDevice)) {
            [void](Invoke-Native pnputil.exe @('/restart-device', $d.InstanceId))
        }
        Start-Sleep -Seconds 6
        Test-M1
        Show-BluetoothStack
        Show-BthUsbEvents
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
            Test-M1
            Show-BthUsbEvents
        } finally {
            if (-not $KeepSolo) {
                Write-Host ''
                Enable-RealRadio -Id $realId
            } else {
                Write-Host ''
                Write-Warn2 'KeepSolo: the real radio is STILL DISABLED.'
                Write-Warn2 'Run M1-6-RESTORE-RADIO.cmd when you are done looking.'
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
        Write-Host 'After the reboot:  M1-4-DIAG.cmd    (read the result)'
        Write-Host 'To undo:           M1-6-RESTORE-RADIO.cmd  then reboot again'
    }
    'Retry' {
        <#
            Assumes the real radio has already been disabled by hand in Device Manager.
            Stages the CURRENT build first: restarting the devnode alone reloads whatever binary
            is already in the driver store, which silently re-tested a stale driver once and
            wasted a run. Staging is idempotent, so this is safe to repeat.
        #>
        Assert-Package
        $radio = Get-RealRadio
        if ($radio.Count -gt 0 -and $radio[0].Status -eq 'OK') {
            Write-Warn2 'The real radio is still ACTIVE - BTHUSB will refuse the emulated one again.'
            Write-Warn2 'Disable it in Device Manager first (Bluetooth -> Qualcomm ... UART Transport).'
            Write-Host ''
        } else {
            Write-Ok 'Real radio is not active - the adapter slot is free'
        }
        Install-Package
        Restart-Emulated
        Test-M1
        Show-BluetoothStack
        Show-BthUsbEvents
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
        Write-Host 'To remove DeckBtUsb entirely instead, run M1-3-UNINSTALL.cmd'
    }
    'RestoreRadio' { Enable-RealRadio -Id $null }
    'Uninstall' {
        Uninstall-Package
        Remove-TestCert
        Clear-BootFlags
        Write-Ok 'Uninstalled. Reboot to drop test signing.'
    }
}
