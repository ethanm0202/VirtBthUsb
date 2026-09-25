<#
    uninstall.ps1 - the authoritative full restore and uninstall for this project.

    GOAL
      Return this machine to its pre-project state: no DeckBtUsb packages, services, devnodes,
      test certificate, test signing, or debug boot flags, and the physical Qualcomm radio back
      under its vendor driver with pairings intact.

    WHAT IT NEVER TOUCHES
      Bluetooth pairings, user data, the Qualcomm vendor package in its DriverStore location, any
      DriverStore package or certificate this project did not create, Secure Boot, or BitLocker.

    PRE-PROJECT EVIDENCE (written by tools\session.ps1 -Prepare)
      recovery/drivers-before.txt    stock driver package set
      recovery/bt-before.txt         stock Bluetooth devices (QCA_SHB\UART_H4, BthMini + QcBluetooth)
      recovery/DeckBtUsbTestCert.cer the exact test certificate that was trusted (matched by thumbprint)
      recovery/bcd-backup.bcd        boot configuration before test signing, fallback only

    USAGE
      tools\session.ps1 -Uninstall
      tools\uninstall.ps1 [-DryRun] [-Force] [-RestoreCodeIntegrity]

    EXIT CODES
      0 restored / already clean    1 action needed or a step failed    2 refused, precondition
#>
[CmdletBinding()]
param(
    [switch] $DryRun,
    [switch] $Force,
    [switch] $RestoreCodeIntegrity,
    [string] $Baseline = 'recovery\baseline\baseline.json'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

. (Join-Path $PSScriptRoot 'deck-state.ps1')

# Aliases kept short for the step code below; the definitions live in deck-state.ps1 so that
# uninstall.ps1 and verify-clean.ps1 cannot disagree about what "clean" means.
$OurInfNames    = $DeckInfNames
$OurProvider    = $DeckProvider
$OurServices    = $DeckServices
$OurDevnodes    = $DeckDevnodes
$VendorStore    = $VendorStoreDir
$VendorBackup   = $VendorBackupDir
$CertFile       = $DeckCertFile
$BcdBackup      = $DeckBcdBackup
$DriversBefore  = $DeckDriversBefore
if (-not $PSBoundParameters.ContainsKey('Baseline')) { $Baseline = $DeckBaselineJson }

$script:exitCode = 0
$script:steps    = @()
$script:needReboot = $false

function Write-Head([string] $m) { Write-Host ''; Write-Host $m -ForegroundColor Cyan; Write-Host ('-' * $m.Length) -ForegroundColor Cyan }
function Write-Ok  ([string] $m) { Write-Host "[+] $m" -ForegroundColor Green }
function Write-Info([string] $m) { Write-Host "[*] $m" -ForegroundColor Gray }
function Write-Warn2([string] $m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Write-Bad ([string] $m) { Write-Host "[-] $m" -ForegroundColor Red }

function Add-Step([string] $name, [string] $state, [string] $detail) {
    $script:steps += [pscustomobject]@{ Step = $name; State = $state; Detail = $detail }
    switch ($state) {
        'DONE' { Write-Ok  "$name - $detail" }
        'SKIP' { Write-Info "$name - SKIP ($detail)" }
        'PLAN' { Write-Info "$name - would $detail" }
        'FAIL' { Write-Bad "$name - FAIL: $detail"; $script:exitCode = 1 }
        default { Write-Info "$name - $state $detail" }
    }
}

# Thin wrappers over deck-state.ps1 so the step code below reads naturally.
function Invoke-Native([string] $exe, [string[]] $Arguments) { return Invoke-DeckNative $exe $Arguments }
function Test-Elevated { return Test-DeckElevated }
function Get-StagedPackages { return Get-DeckStagedPackages }
function Get-DeviceInfo([string] $instanceLike, [switch] $Like) {
    if ($Like) { return Get-DeckDevice $instanceLike -Like } else { return Get-DeckDevice $instanceLike }
}
function Get-PairedCount { $p = Get-DeckPairedDevices; if ($null -eq $p) { return $null } return $p.Count }
function Get-TestSigning { return Get-DeckTestSigning }
function Get-HvciEnabled { return Get-DeckHvci }
function Get-OurCertThumbprints { return Get-DeckCertThumbprints }

# --- steps -----------------------------------------------------------------------------------

function Step-Disarm {
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'
    if (-not (Test-Path -LiteralPath $key)) { Add-Step 'Disarm arm token' 'SKIP' 'DeckBtUsb\Parameters absent'; return }
    $cur = (Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue).Enabled
    if ($cur -eq 0) { Add-Step 'Disarm arm token' 'SKIP' 'Enabled already 0'; return }
    if ($DryRun) { Add-Step 'Disarm arm token' 'PLAN' "set Enabled=0 (currently '$cur')"; return }
    Set-ItemProperty -LiteralPath $key -Name 'Enabled' -Value 0 -Type DWord -Force
    $now = (Get-ItemProperty -LiteralPath $key).Enabled
    if ($now -eq 0) { Add-Step 'Disarm arm token' 'DONE' 'Enabled=0' } else { Add-Step 'Disarm arm token' 'FAIL' "readback $now" }
}

function Step-RemoveDevnodes {
    foreach ($id in $OurDevnodes) {
        $d = Get-DeviceInfo $id
        if (-not $d) { Add-Step "Remove devnode $id" 'SKIP' 'not present'; continue }
        if ($DryRun) { Add-Step "Remove devnode $id" 'PLAN' 'pnputil /remove-device /subtree'; continue }
        $r = Invoke-Native pnputil.exe @('/remove-device', $d.InstanceId, '/subtree')
        if ($r.Code -ne 0) {
            Add-Step "Remove devnode $id" 'FAIL' "removal failed or needs restart (rc=$($r.Code))"
            return
        }
        Start-Sleep -Milliseconds 500
        if (Get-DeviceInfo $id) { Add-Step "Remove devnode $id" 'FAIL' "still present (rc=$($r.Code))" }
        else { Add-Step "Remove devnode $id" 'DONE' 'removed' }
    }
}

function Step-RestoreRadio {
    if ($DryRun) {
        Add-Step 'Restore physical radio' 'PLAN' 'verify vendor health or run the guarded radio handback'
        return
    }
    try {
        [void](Assert-DeckHealthyVendorRadio)
        Add-Step 'Restore physical radio' 'SKIP' 'vendor radio and child are already healthy'
        return
    } catch {
        if ($_.Exception.Message -notlike 'REFUSAL:*') {
            Add-Step 'Restore physical radio' 'FAIL' $_.Exception.Message
            return
        }
    }
    $vendorInf = Join-Path $VendorStore 'qcbtuart.inf'
    if (-not (Test-Path -LiteralPath $vendorInf)) {
        $backupInf = Join-Path $VendorBackup 'qcbtuart.inf'
        if (-not (Test-Path -LiteralPath $backupInf)) {
            Add-Step 'Restore physical radio' 'FAIL' 'vendor package missing from DriverStore and recovery backup'
            return
        }
        $result = Invoke-Native pnputil.exe @('/add-driver', $backupInf)
        if ($result.Code -ne 0) {
            Add-Step 'Restore physical radio' 'FAIL' "vendor staging failed or needs restart (rc=$($result.Code))"
            return
        }
    }
    & (Join-Path $PSHOME 'powershell.exe') -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'restore-radio.ps1')
    if ($LASTEXITCODE -ne 0) {
        Add-Step 'Restore physical radio' 'FAIL' 'guarded handback did not complete; preserving trust and signing'
        return
    }
    try {
        $healthy = Assert-DeckHealthyVendorRadio
        Add-Step 'Restore physical radio' 'DONE' "$VendorService owns $($healthy.Radio); healthy child $($healthy.Child)"
    } catch { Add-Step 'Restore physical radio' 'FAIL' $_.Exception.Message }
}

function Step-RemovePackages {
    $pkgs = Get-StagedPackages
    $ours = @($pkgs | Where-Object { $_.IsOurs })
    $suspects = @($pkgs | Where-Object { -not $_.IsOurs -and ($OurInfNames -contains $_.OriginalName.ToLowerInvariant()) })
    foreach ($s in $suspects) {
        Write-Warn2 "SKIPPING $($s.Published) ($($s.OriginalName), provider '$($s.Provider)') - name matches but provider is not '$OurProvider'"
    }
    if ($ours.Count -eq 0) { Add-Step 'Remove staged packages' 'SKIP' 'no project packages are staged'; return }
    if ($DryRun) {
        Add-Step 'Remove staged packages' 'PLAN' ("delete " + ($ours | ForEach-Object { "$($_.Published) [$($_.OriginalName)]" }) -join ', ')
        return
    }
    foreach ($p in $ours) {
        $r = Invoke-Native pnputil.exe @('/delete-driver', $p.Published, '/uninstall', '/force')
        if ($r.Code -ne 0) {
            Add-Step 'Remove staged packages' 'FAIL' "$($p.Published) rc=$($r.Code)$(if ($r.Code -eq 3010) { ' reboot pending' })"
            return
        }
    }
    $left = @((Get-StagedPackages) | Where-Object { $_.IsOurs })
    if ($left.Count -eq 0) { Add-Step 'Remove staged packages' 'DONE' "removed $($ours.Count) package(s)" }
    else { Add-Step 'Remove staged packages' 'FAIL' ("remaining: " + (($left | ForEach-Object { $_.Published }) -join ', ')) }
}

function Step-RemoveServices {
    if ($DryRun) {
        Add-Step 'Remove services' 'PLAN' 'remove project services only after package removal and healthy vendor ownership'
        return
    }
    # A failed uninstall must never strand a devnode with a deleted service.
    $remaining = @((Get-StagedPackages) | Where-Object { $_.IsOurs })
    try { $healthy = Assert-DeckHealthyVendorRadio }
    catch { Add-Step 'Remove services' 'FAIL' "preserving services: $($_.Exception.Message)"; return }
    if ($remaining.Count -gt 0) {
        Add-Step 'Remove services' 'FAIL' ("preserving services: remaining packages=[" + (($remaining | ForEach-Object { $_.Published }) -join ', ') + "]; radio=$($healthy.Radio)")
        return
    }
    foreach ($svc in $OurServices) {
        $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$svc"
        $present = Test-Path -LiteralPath $key
        if (-not $present) { Add-Step "Remove service $svc" 'SKIP' 'not present'; continue }
        [void](Invoke-Native sc.exe @('stop', $svc))
        $r = Invoke-Native sc.exe @('delete', $svc)
        if (Test-Path -LiteralPath $key) {
            try { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction Stop } catch { }
        }
        if (Test-Path -LiteralPath $key) { Add-Step "Remove service $svc" 'FAIL' "key remains (sc rc=$($r.Code)); a reboot may be required" }
        else { Add-Step "Remove service $svc" 'DONE' 'service and Parameters removed' }
    }
}

function Step-RemoveCert {
    try { $thumbs = @(Get-OurCertThumbprints) }
    catch { Add-Step 'Remove test certificate' 'FAIL' $_.Exception.Message; return }
    if ($thumbs.Count -eq 0) {
        Add-Step 'Remove test certificate' 'FAIL' 'saved project thumbprint unavailable; certificate trust remains unresolved (no subject-match deletion)'
        return
    }
    $targets = @(Get-DeckCertPresence)
    if ($targets.Count -eq 0) { Add-Step 'Remove test certificate' 'SKIP' 'exact project thumbprint absent from Root and TrustedPublisher'; return }
    if ($DryRun) { Add-Step 'Remove test certificate' 'PLAN' "remove $($targets.Count) exact project certificate entr(ies)"; return }
    foreach ($t in $targets) {
        Remove-Item -LiteralPath "Cert:\LocalMachine\$($t.Store)\$($t.Thumbprint)" -Force -ErrorAction Stop
    }
    $still = @(Get-DeckCertPresence)
    if ($still.Count -eq 0) { Add-Step 'Remove test certificate' 'DONE' "removed $($targets.Count) entr(ies)" }
    else { Add-Step 'Remove test certificate' 'FAIL' "$($still.Count) entr(ies) remain" }
}

function Step-TestSigningOff {
    $state = Get-TestSigning
    if ($state -eq 'off') { Add-Step 'Disable test signing' 'SKIP' 'already off'; return }
    if ($DryRun) { Add-Step 'Disable test signing' 'PLAN' "bcdedit /set testsigning off (currently $state)"; return }
    $r = Invoke-Native bcdedit.exe @('/set', 'testsigning', 'off')
    if ((Get-TestSigning) -eq 'off') {
        $script:needReboot = $true
        Add-Step 'Disable test signing' 'DONE' 'set to off; a reboot is required for it to take effect'
    } else {
        Add-Step 'Disable test signing' 'FAIL' "off was not verified (bcdedit rc=$($r.Code)): $($r.Output.Trim()). Stop and report this output."
    }
}

<#
    Step-TestSigningOff above reverts testsigning. This step reverts nocrashautoreboot
    and bootstatuspolicy if present. Stock Windows omits both elements.
#>
function Step-ClearBootFlags {
    $boot = Get-DeckBootFlags
    if (-not $boot.Readable) {
        if ($DryRun) {
            Add-Step 'Clear debug boot flags' 'PLAN' 'read and clear them (bcdedit needs elevation, so a dry run cannot see their state)'
        } else {
            Add-Step 'Clear debug boot flags' 'FAIL' 'bcdedit unreadable while elevated; state unknown. Stop and report this.'
        }
        return
    }
    if ($boot.Leftovers.Count -eq 0) { Add-Step 'Clear debug boot flags' 'SKIP' $boot.Detail; return }
    if ($DryRun) { Add-Step 'Clear debug boot flags' 'PLAN' "bcdedit /deletevalue for: $($boot.Leftovers -join ' ')"; return }

    $failed = @()
    foreach ($flag in $DeckDebugBootFlags) {
        if (-not @($boot.Leftovers | Where-Object { $_ -like "$flag=*" }).Count) { continue }
        $r = Invoke-Native bcdedit.exe @('/deletevalue', '{current}', $flag)
        if ($r.Code -ne 0) { $failed += "$flag rc=$($r.Code) $($r.Output.Trim())" }
    }

    $after = Get-DeckBootFlags
    if ($after.Readable -and $after.Leftovers.Count -eq 0) {
        $script:needReboot = $true
        Add-Step 'Clear debug boot flags' 'DONE' "removed $($boot.Leftovers -join ' '); a reboot is required for it to take effect"
    } else {
        $detail = "still present: $($after.Leftovers -join ' ')"
        if ($failed.Count) { $detail += "; errors: " + ($failed -join '; ') }
        Add-Step 'Clear debug boot flags' 'FAIL' $detail
    }
}

function Step-CodeIntegrity {
    $state = Get-HvciEnabled
    if (-not $RestoreCodeIntegrity) {
        Add-Step 'Re-enable HVCI' 'SKIP' "not requested (current: $state); pass -RestoreCodeIntegrity to change it"
        return
    }
    if ($DryRun) { Add-Step 'Re-enable HVCI' 'PLAN' "set HypervisorEnforcedCodeIntegrity Enabled=1 (currently $state)"; return }
    $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
    New-Item -Path $k -Force | Out-Null
    Set-ItemProperty -LiteralPath $k -Name 'Enabled' -Value 1 -Type DWord -Force
    $script:needReboot = $true
    Add-Step 'Re-enable HVCI' 'DONE' 'Enabled=1; reboot required. NOTE: this blocks self-signed kernel drivers, so this project cannot run again until it is turned back off'
}

# --- verification ----------------------------------------------------------------------------

function Invoke-Verification {
    Write-Head 'VERIFICATION'
    $checks = Get-DeckChecks
    foreach ($c in $checks) {
        if ($c.Pass -eq $true) { Write-Ok "$($c.Name): $($c.Detail)" }
        elseif ($null -eq $c.Pass) { Write-Warn2 "$($c.Name): UNDETERMINED ($($c.Detail))"; if ($script:exitCode -eq 0) { $script:exitCode = 2 } }
        else { Write-Bad "$($c.Name): $($c.Detail)"; $script:exitCode = 1 }
    }
    return $checks
}

# --- main ------------------------------------------------------------------------------------

Write-Host ''
Write-Host 'Uninstall driver - return this machine to its pre-project state' -ForegroundColor White
Write-Host '  Removes: project driver packages, services, devnodes, test certificate, test signing,'
Write-Host '           and the nocrashautoreboot / bootstatuspolicy debug boot flags.'
Write-Host '  Restores: the physical Qualcomm radio to its vendor driver (QcBluetooth).'
Write-Host '  Never touches: pairings, user data, the vendor package, foreign packages or certs,'
Write-Host '                 Secure Boot, or BitLocker.'
Write-Host ''

if (-not $DryRun -and -not (Test-Elevated)) {
    Write-Bad 'Live restore requires administrator rights. Run from an elevated console, or add -DryRun to plan only.'
    exit 2
}

foreach ($f in @($DriversBefore, $CertFile, $BcdBackup)) {
    if (Test-Path -LiteralPath $f) { Write-Info "pre-project evidence present: $(Split-Path -Leaf $f)" }
    else { Write-Warn2 "pre-project evidence MISSING: $f" }
}
if (-not (Test-Path -LiteralPath $Baseline)) {
    Write-Warn2 "No baseline at $Baseline - paired-device comparison will be skipped. Run tools\capture-baseline.ps1 to create one."
}

Write-Head 'PLAN'
$pkgs = Get-StagedPackages
$ourPkgs = @($pkgs | Where-Object { $_.IsOurs })
Write-Info "project packages staged: $(if ($ourPkgs.Count) { ($ourPkgs | ForEach-Object { "$($_.Published)[$($_.OriginalName)]" }) -join ' ' } else { 'none' })"
Write-Info "foreign packages left untouched: $(@($pkgs | Where-Object { -not $_.IsOurs }).Count)"
$node = Get-DeviceInfo $RadioAcpiId
Write-Info "radio node: $(if ($node) { "$($node.InstanceId) service=$($node.Service) status=$($node.Status)" } else { 'absent' })"
Write-Info "paired devices now: $(Get-PairedCount)"
Write-Info "test signing now: $(Get-TestSigning);  HVCI now: $(Get-HvciEnabled)"

if (-not $DryRun -and -not $Force) {
    Write-Host ''
    $answer = Read-Host 'Type RESTORE to proceed (anything else aborts)'
    if ($answer -ne 'RESTORE') {
        Write-Warn2 'Aborted by operator. Nothing was changed.'
        exit 1
    }
}

function Invoke-UninstallSteps {
    foreach ($step in @('Step-Disarm', 'Step-RemoveDevnodes', 'Step-RestoreRadio',
                        'Step-RemovePackages', 'Step-RemoveServices', 'Step-RemoveCert',
                        'Step-TestSigningOff', 'Step-ClearBootFlags', 'Step-CodeIntegrity')) {
        try { & $step } catch { Add-Step $step 'FAIL' $_.Exception.Message }
        if ($script:exitCode -ne 0) {
            Write-Warn2 'Uninstall stopped at the failed step; subsequent cleanup was not attempted.'
            return
        }
    }
}

Write-Head $(if ($DryRun) { 'STEPS (DRY RUN - nothing is changed)' } else { 'STEPS' })
Invoke-UninstallSteps

if (-not $DryRun -and $script:exitCode -eq 0) { [void](Invoke-Verification) }

Write-Head 'SUMMARY'
$script:steps | ForEach-Object { Write-Host ("  {0,-34} {1,-5} {2}" -f $_.Step, $_.State, $_.Detail) }

if ($script:exitCode -eq 0) {
    Write-Host ''
    Write-Info "Secure Boot: $(try { if (Confirm-SecureBootUEFI) { 'on' } else { 'off' } } catch { 'unknown' }) - owner/firmware decision, not changed here."
    Write-Info "BitLocker (system drive): $(try { (Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop).ProtectionStatus } catch { 'unknown' }) - not changed here."
    Write-Info "HVCI: $(Get-HvciEnabled)$(if (-not $RestoreCodeIntegrity) { ' - pass -RestoreCodeIntegrity to re-enable it' })"
}

Write-Host ''
if ($DryRun) {
    Write-Ok 'DRY RUN complete. Nothing was changed. Re-run without -DryRun to restore.'
} elseif ($script:exitCode -eq 0) {
    Write-Ok 'RESTORED. This machine matches its pre-project state on every checked dimension.'
    if ($script:needReboot) { Write-Warn2 'A reboot is required to finish clearing test signing / code integrity.' }
} else {
    Write-Bad 'NOT fully restored. Fix the FAIL lines above, then run tools\verify-clean.ps1.'
    Write-Host ''
    Write-Host '  Manual recovery references:' -ForegroundColor Yellow
    Write-Host "    vendor package (DriverStore) : $VendorStore"
    Write-Host "    vendor package (backup)      : $VendorBackup"
    Write-Host "    re-stage vendor driver       : pnputil /add-driver `"$VendorStore\qcbtuart.inf`" /install"
    Write-Host "    pre-project BCD store        : $BcdBackup"
    Write-Host "    restore BCD (last resort)    : bcdedit /import `"$BcdBackup`""
    Write-Host '    radio restore                : tools\session.ps1 -Stop'
}

exit $script:exitCode
