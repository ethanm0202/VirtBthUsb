<#
    deck-state.ps1 - shared, read-only knowledge of this project's footprint.

    Dot-sourced by tools\uninstall.ps1, tools\verify-clean.ps1 and the session scripts so that
    "clean" has exactly one definition. Nothing in this file mutates anything.

    Pre-project evidence written by tools\session.ps1 -Prepare:
      recovery/drivers-before.txt    stock driver package set
      recovery/bt-before.txt         stock Bluetooth devices (QCA_SHB\UART_H4, BthMini + QcBluetooth)
      recovery/DeckBtUsbTestCert.cer the exact test certificate that was trusted
      recovery/bcd-backup.bcd        boot configuration before test signing
#>

$script:DeckRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

$DeckInfNames    = @('deckbtusb.inf', 'deckbtflt.inf', 'isotest.inf')
$DeckProvider    = 'DeckBtUsb'
$DeckServices    = @('DeckBtUsb', 'DeckBtFlt', 'DeckBtIsoTest', 'DeckBtIsoFlt')
$DeckServicesRegRoot  = 'HKLM:\SYSTEM\CurrentControlSet\Services'
$DeckDevnodes    = @('ROOT\DEVGEN\DECKBTUSB', 'ROOT\DEVGEN\DECKBTISOTEST')
$RadioAcpiId     = 'ACPI\QCOM2066'
$RadioChildLike  = 'QCA_SHB\UART_H4*'
$VendorService   = 'QcBluetooth'
$VendorStoreDir  = & {
    $fileRepo = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
    if (Test-Path -LiteralPath $fileRepo) {
        $candidates = @(Get-ChildItem -LiteralPath $fileRepo -Directory -Filter 'qcbtuart.inf_amd64_*' |
            Sort-Object LastWriteTime -Descending)
        if ($candidates.Count -gt 0) {
            return $candidates[0].FullName
        }
    }
    return Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository\qcbtuart.inf_amd64_default'
}
$EwdkDir          = if ($env:EWDK) { $env:EWDK } else { 'C:\EWDK' }
$DevGenPath       = Join-Path $EwdkDir 'Program Files\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'
$VendorBackupDir = Join-Path $script:DeckRoot 'recovery\baseline\vendor-qcbtuart'
$DeckCertFile    = Join-Path $script:DeckRoot 'recovery\DeckBtUsbTestCert.cer'
$DeckBcdBackup   = Join-Path $script:DeckRoot 'recovery\bcd-backup.bcd'
$DeckDriversBefore = Join-Path $script:DeckRoot 'recovery\drivers-before.txt'
$DeckBtBefore    = Join-Path $script:DeckRoot 'recovery\bt-before.txt'
$DeckBaselineDir = Join-Path $script:DeckRoot 'recovery\baseline'
$DeckBaselineJson = Join-Path $DeckBaselineDir 'baseline.json'

function Invoke-DeckNative([string] $Exe, [string[]] $Arguments) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $old }
    return [pscustomobject]@{ Code = $code; Output = ($out | Out-String) }
}

function Test-DeckElevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]::new($id)).IsInRole(
        [Security.Principal.WindowsBuiltinRole]::Administrator)
}

function Get-DeckPackageList([string] $Text) {
    # pnputil's text is localized. Never interpret an unrecognized response as an empty store.
    if ([string]::IsNullOrWhiteSpace($Text)) { throw 'Unsupported pnputil /enum-drivers output: empty response.' }
    $lines = @($Text -split '\r?\n' | Where-Object { $_.Trim() })
    if ($lines.Count -and $lines[0].Trim() -eq 'Microsoft PnP Utility') { $lines = @($lines | Select-Object -Skip 1) }
    if ($lines.Count -eq 0) { throw 'Unsupported pnputil /enum-drivers output: no package records.' }
    $out = @()
    $fields = @{}
    $label = $null
    foreach ($line in $lines) {
        if ($line -notmatch '^\s*([^:]+):\s*(.*?)\s*$') {
            # Newer Windows emits additional package attributes on indented continuation lines.
            if ($label -eq 'Attributes' -and $line -match '^\s+\S') {
                $fields[$label] += ' ' + $line.Trim()
                continue
            }
            throw "Unsupported pnputil /enum-drivers output: $line"
        }
        $label = $Matches[1].Trim()
        $value = $Matches[2]
        if ($label -eq 'Published Name') {
            if ($fields.Count) {
                $out += ConvertTo-DeckPackage $fields
                $fields = @{}
            }
        } elseif (-not $fields.ContainsKey('Published Name')) {
            throw "Unsupported pnputil /enum-drivers output: $line"
        }
        if ($fields.ContainsKey($label)) { throw "Unsupported pnputil /enum-drivers output: duplicate $label." }
        $fields[$label] = $value
    }
    if (-not $fields.Count) { throw 'Unsupported pnputil /enum-drivers output: no package records.' }
    $out += ConvertTo-DeckPackage $fields
    return $out
}

function ConvertTo-DeckPackage([hashtable] $Fields) {
    foreach ($key in @('Published Name', 'Original Name', 'Provider Name', 'Driver Version')) {
        if (-not $Fields.ContainsKey($key) -or [string]::IsNullOrWhiteSpace($Fields[$key])) {
            throw "Unsupported pnputil /enum-drivers output: missing $key."
        }
    }
    $pub = $Fields['Published Name']
    $orig = $Fields['Original Name']
    if ($pub -notmatch '^oem\d+\.inf$' -or $orig -notmatch '^[^\\/: ]+\.inf$') {
        throw 'Unsupported pnputil /enum-drivers output: invalid package identity.'
    }
    return [pscustomobject]@{
        Published    = $pub
        OriginalName = $orig
        Provider     = $Fields['Provider Name']
        Version      = $Fields['Driver Version']
        IsOurs       = (($DeckInfNames -contains $orig.ToLowerInvariant()) -and ($Fields['Provider Name'] -eq $DeckProvider))
    }
}

function Get-DeckStagedPackages {
    $result = Invoke-DeckNative pnputil.exe @('/enum-drivers')
    if ($result.Code -ne 0) { throw "pnputil /enum-drivers failed (rc=$($result.Code)): $($result.Output)" }
    return Get-DeckPackageList $result.Output
}

function Get-DeckStockPackages {
    # The pre-project package set. Anything allowlisted that is absent here belongs to this project.
    if (-not (Test-Path -LiteralPath $DeckDriversBefore)) { return @() }
    return Get-DeckPackageList (Get-Content -LiteralPath $DeckDriversBefore -Raw)
}

# Exact devnode existence, including non-present devnodes. Call only inside a bounded worker.
function Get-DeckExactRoot {
    $id = 'ROOT\DEVGEN\DECKBTUSB'
    $errors = @()
    $nodes = @(Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue -ErrorVariable errors)
    foreach ($errorRecord in $errors) {
        if ($errorRecord.CategoryInfo.Category -ne 'ObjectNotFound') { throw $errorRecord }
    }
    if ($nodes.Count -eq 0) { return $null }
    if ($nodes.Count -ne 1 -or $nodes[0].InstanceId -ine $id) {
        throw 'REFUSAL: exact devnode identity is ambiguous; preserving all devnodes.'
    }
    return $nodes[0]
}

<#
    Get-DeckServiceRecord - query service registration and driver parameters.
    Why fail-closed: an unreadable service key (access denied, unreadable hive) must throw
    so it is never misreported as absent. An absent key is the only silent $null return.
#>
function Get-DeckServiceRecord([string] $Name, [string] $ServicesRoot = $DeckServicesRegRoot) {
    $keyPath = Join-Path $ServicesRoot $Name
    if (-not (Test-Path -LiteralPath $keyPath)) {
        return $null
    }
    $props = Get-ItemProperty -LiteralPath $keyPath -ErrorAction Stop
    $typeProp = $props.PSObject.Properties['Type']
    $type = if ($null -ne $typeProp) { $typeProp.Value } else { $null }
    $startProp = $props.PSObject.Properties['Start']
    $start = if ($null -ne $startProp) { $startProp.Value } else { $null }
    $imageProp = $props.PSObject.Properties['ImagePath']
    $imagePath = if ($null -ne $imageProp -and $null -ne $imageProp.Value) { [string]$imageProp.Value } else { $null }

    $paramsPath = Join-Path $keyPath 'Parameters'
    $hasParams = Test-Path -LiteralPath $paramsPath
    # An empty @() unrolls to nothing through an if-expression, which would land here as $null and
    # make every downstream count/contains test lie. Seed the array, then overwrite it.
    $uartNames = @()
    if ($hasParams) {
        $paramProps = Get-ItemProperty -LiteralPath $paramsPath -ErrorAction Stop
        $uartNames = @($paramProps.PSObject.Properties |
            Where-Object { $_.Name -like 'Uart*' } |
            ForEach-Object { [string]$_.Name } |
            Sort-Object)
    }

    return [pscustomobject]@{
        Name           = $Name
        Type           = $type
        Start          = $start
        ImagePath      = $imagePath
        HasParameters  = [bool]$hasParams
        UartValueNames = [string[]]$uartNames
    }
}

<#
    Test-DeckOwnedServiceRemnant - pure predicate checking if a service record is a valueless remnant.
    Prevents a failure where an uninstalled or pre-armed service key (such as Services\DeckBtUsb
    with only a Parameters subkey, and no Type, Start, ImagePath, or ErrorControl values)
    blocked AddService with "Failed to get configuration of service" / Error 2.
    Why fail-closed: any configured service carrying a Type or ImagePath, or any service name outside
    $DeckServices, must never be treated as a remnant and will return $false to prevent accidental deletion.
#>
function Test-DeckOwnedServiceRemnant([object] $Record) {
    if ($null -eq $Record) { return $false }
    try {
        $name = $Record.Name
        $type = $Record.Type
        $imagePath = $Record.ImagePath
    } catch {
        return $false
    }
    if ($null -eq $name) { return $false }
    $nameStr = [string]$name
    $isOwnedName = $false
    foreach ($svc in $DeckServices) {
        if ($svc -ieq $nameStr) {
            $isOwnedName = $true
            break
        }
    }
    if (-not $isOwnedName) { return $false }

    if ($null -ne $type) { return $false }
    if ($null -ne $imagePath -and -not [string]::IsNullOrWhiteSpace([string]$imagePath)) { return $false }

    return $true
}

<#
    Remove-DeckOwnedServiceRemnant - bounded removal of a valueless owned service remnant.
    Prevents a failure where a valueless Services\DeckBtUsb key with a Parameters subkey
    blocked AddService with Error 2.
    Why fail-closed: verifies that the target is strictly an owned remnant via Test-DeckOwnedServiceRemnant.
    If the key is configured or foreign, throws a refusal containing 'not a remnant' without calling sc.exe
    or mutating any registry state. An absent key is idempotent.
#>
function Remove-DeckOwnedServiceRemnant([string] $Name, [string] $ServicesRoot = $DeckServicesRegRoot, [scriptblock] $NativeRunner = $null) {
    $runner = if ($null -ne $NativeRunner) { $NativeRunner } else { { param($Exe, $Arguments) Invoke-DeckNative $Exe $Arguments } }
    $record = Get-DeckServiceRecord $Name $ServicesRoot
    if ($null -eq $record) {
        return [pscustomobject]@{
            Removed           = $true
            MarkedForDeletion = $false
            Detail            = 'already absent'
        }
    }

    if (-not (Test-DeckOwnedServiceRemnant $record)) {
        throw "REFUSAL: service '$Name' is not a remnant (Type=$($record.Type), ImagePath='$($record.ImagePath)') and will not be removed."
    }

    $keyPath = Join-Path $ServicesRoot $Name
    $paramsPath = Join-Path $keyPath 'Parameters'

    # 1. remove Parameters subkey recursively when present
    if (Test-Path -LiteralPath $paramsPath) {
        Remove-Item -LiteralPath $paramsPath -Recurse -Force
    }

    # 2. call runner exactly once
    # ERROR_SERVICE_DOES_NOT_EXIST (1060) is expected for a registry-only orphan.
    # Only the remnant validated above may then be removed; absence is verified below.
    $markedForDeletion = $false
    $scResult = & $runner 'sc.exe' @('delete', $Name)
    if ($scResult.Code -eq 0) {
        if ($scResult.Output -match '(?i)marked for deletion') {
            $markedForDeletion = $true
        }
    } elseif ($scResult.Code -eq 1072 -or $scResult.Output -match '(?i)marked for deletion') {
        $markedForDeletion = $true
    } elseif ($scResult.Code -ne 1060) {
        throw "Service remnant deletion failed (Code=$($scResult.Code)): $($scResult.Output)"
    }

    # 3. remove service key itself recursively when it still exists
    if (Test-Path -LiteralPath $keyPath) {
        Remove-Item -LiteralPath $keyPath -Recurse -Force
    }

    # 4. re-read record and return
    $reread = Get-DeckServiceRecord $Name $ServicesRoot
    $isRemoved = ($null -eq $reread)
    $detail = if ($isRemoved) {
        'removed'
    } else {
        "surviving Start=$($reread.Start) ImagePath='$($reread.ImagePath)' HasParameters=$($reread.HasParameters)"
    }

    return [pscustomobject]@{
        Removed           = $isRemoved
        MarkedForDeletion = $markedForDeletion
        Detail            = $detail
    }
}

<#
    Get-DeckCheckedDevices - query PnP devices matching an instance ID pattern.
    Why fail-closed: unexpected PnP errors must propagate, never be swallowed as empty results.
#>
function Get-DeckCheckedDevices([string] $Pattern, [switch] $PresentOnly) {
    $errors = @()
    $nodes = @(Get-PnpDevice -InstanceId $Pattern -PresentOnly:$PresentOnly -ErrorAction SilentlyContinue -ErrorVariable errors)
    foreach ($e in $errors) {
        if ($e.CategoryInfo.Category -ne 'ObjectNotFound') { throw $e }
    }
    return $nodes
}

<#
    Get-DeckNodeProperty - query a DEVPKEY property on a device instance.
    Why fail-closed: property access failures must throw unless explicitly marked optional.
#>
function Get-DeckNodeProperty([string] $Id, [string] $Key, [switch] $Optional) {
    try { return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data }
    catch {
        if ($Optional -and $_.CategoryInfo.Category -eq 'ObjectNotFound') { return $null }
        throw
    }
}

# pnputil /install can succeed even when Windows refuses to load the driver.
# Check this before consuming completion breadcrumbs; those may be stale.
function Assert-DeckNoDeviceProblem($Node) {
    if ($null -eq $Node) { return }
    $hasProblem = "$($Node.Problem)" -notin @('', '0', 'CM_PROB_NONE')
    if ($Node.Status -eq 'Error' -or $hasProblem) {
        throw "Device failed to start: $($Node.InstanceId); Service=$($Node.Service); Status=$($Node.Status); Problem=$($Node.Problem); ProblemStatus=$($Node.ProblemStatus). Check Windows INF\setupapi.dev.log and Microsoft-Windows-CodeIntegrity/Operational for driver-load/signing details. Package installation and Authenticode verification do not prove driver load."
    }
}

<#
    Assert-DeckHealthyVendorRadio - verify the physical Bluetooth controller is vendor-owned and healthy.
    Why fail-closed: any missing, duplicate, non-vendor, or degraded device status must refuse
    to guarantee the physical radio is never targeted or left in an unstable state.
#>
function Assert-DeckHealthyVendorRadio {
    $radios = @(Get-DeckCheckedDevices 'ACPI\QCOM2066*' -PresentOnly)
    $children = @(Get-DeckCheckedDevices 'QCA_SHB\UART_H4*' -PresentOnly)
    if ($radios.Count -ne 1 -or $children.Count -ne 1) {
        throw 'REFUSAL: radio/child identity is missing or ambiguous; expected exactly one present ACPI\QCOM2066 and QCA_SHB\UART_H4.'
    }
    $radio = $radios[0]
    $child = $children[0]
    $radioService = Get-DeckNodeProperty $radio.InstanceId DEVPKEY_Device_Service
    $childService = Get-DeckNodeProperty $child.InstanceId DEVPKEY_Device_Service
    if ($radioService -ne 'QcBluetooth' -or $radio.Status -ne 'OK' -or
        "$($radio.Problem)" -notin @('', '0', 'CM_PROB_NONE') -or
        $childService -ne 'BthMini' -or $child.Status -ne 'OK' -or
        "$($child.Problem)" -notin @('', '0', 'CM_PROB_NONE')) {
        throw "REFUSAL: physical radio is not healthy/vendor-owned: radio=$radioService/$($radio.Status)/$($radio.Problem), child=$childService/$($child.Status)/$($child.Problem). No radio changes will be attempted."
    }
    return [pscustomobject]@{
        Radio        = $radio.InstanceId
        RadioService = $radioService
        RadioStatus  = $radio.Status
        Child        = $child.InstanceId
        ChildService = $childService
        ChildStatus  = $child.Status
    }
}

function Get-DeckDevice([string] $InstanceId, [switch] $Like) {
    $all = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue)
    $d = if ($Like) { $all | Where-Object { $_.InstanceId -like $InstanceId } | Select-Object -First 1 }
         else       { $all | Where-Object { $_.InstanceId -like "$InstanceId*" } | Select-Object -First 1 }
    if (-not $d) { return $null }
    $svc = $null
    try { $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction Stop).Data } catch { }
    return [pscustomobject]@{
        InstanceId = $d.InstanceId; Status = $d.Status; Problem = $d.Problem
        Service = $svc; Name = $d.FriendlyName
    }
}

function Get-DeckPairedDevices {
    try {
        return @(Get-PnpDevice -PresentOnly -Class Bluetooth -ErrorAction Stop |
                 Where-Object { $_.InstanceId -like 'BTHENUM\DEV_*' -or $_.InstanceId -like 'BTHLE\DEV_*' } |
                 ForEach-Object { [pscustomobject]@{ Name = $_.FriendlyName; InstanceId = $_.InstanceId; Status = $_.Status } })
    } catch { return $null }
}

<#
    bcdedit reports this element as "Yes"/"No", not "on"/"off", and omits it entirely when it has
    never been set. Everything downstream compares against 'on'/'off', so normalize here once:
    a raw string comparison against 'off' would have made a successfully disabled machine report
    'no' and fail the "test signing off" check forever.
#>
function ConvertTo-DeckTestSigningState([string] $Word) {
    switch ($Word.Trim().ToLowerInvariant()) {
        'yes'   { return 'on' }
        'on'    { return 'on' }
        'true'  { return 'on' }
        '1'     { return 'on' }
        'no'    { return 'off' }
        'off'   { return 'off' }
        'false' { return 'off' }
        '0'     { return 'off' }
        ''      { return 'off' }
        default { return 'unknown' }
    }
}

function Get-DeckTestSigning {
    $r = Invoke-DeckNative bcdedit.exe @('/enum', '{current}')
    if ($r.Code -ne 0) { return 'unknown' }   # typically access denied when not elevated
    $m = [regex]::Match($r.Output, '(?im)^\s*testsigning\s+(\w+)')
    if ($m.Success) { return ConvertTo-DeckTestSigningState $m.Groups[1].Value }
    return 'off'   # element absent means it was never enabled
}

# Every boot element Set-BootFlags mutates other than testsigning. Stock Windows
# omits both entirely, so presence is the leftover signal and the value is irrelevant.
$DeckDebugBootFlags = @('nocrashautoreboot', 'bootstatuspolicy')

<#
    Get-DeckBootFlags - one bcdedit read covering every boot element this project mutates.

    Set-BootFlags sets three: testsigning, nocrashautoreboot, bootstatuspolicy. Only testsigning
    was ever reverted or verified, so a host could pass "is this machine stock?" while still
    carrying the other two.

    nocrashautoreboot in particular outranks HKLM\...\Control\CrashControl\AutoReboot, so a host
    left with it set halts on a bugcheck forever regardless of the registry value.

    Readable is $false when bcdedit could not run at all - non-elevation is the normal cause.
    Callers MUST map that to UNDETERMINED and never to a pass.
#>
function Get-DeckBootFlags {
    $r = Invoke-DeckNative bcdedit.exe @('/enum', '{current}')
    if ($r.Code -ne 0) {
        return [pscustomobject]@{
            Readable    = $false
            TestSigning = 'unknown'
            Leftovers   = @()
            Detail      = 'bcdedit unreadable (elevation required)'
        }
    }

    $m = [regex]::Match($r.Output, '(?im)^\s*testsigning\s+(\w+)')
    $ts = if ($m.Success) { ConvertTo-DeckTestSigningState $m.Groups[1].Value } else { 'off' }

    $leftovers = @()
    foreach ($flag in $DeckDebugBootFlags) {
        $f = [regex]::Match($r.Output, "(?im)^\s*$flag\s+(\S+)")
        if ($f.Success) { $leftovers += "$flag=$($f.Groups[1].Value)" }
    }

    return [pscustomobject]@{
        Readable    = $true
        TestSigning = $ts
        Leftovers   = $leftovers
        Detail      = if ($leftovers.Count) { $leftovers -join ' ' } else { ($DeckDebugBootFlags -join ' and ') + ' absent' }
    }
}

function Get-DeckHvci {
    $k = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity'
    if (-not (Test-Path -LiteralPath $k)) { return 'not configured' }
    $v = (Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue).Enabled
    if ($null -eq $v) { return 'not configured' }
    return "$v"
}

function Get-DeckSecureBoot {
    try { if (Confirm-SecureBootUEFI) { return 'on' } else { return 'off' } } catch { return 'unknown' }
}

function Get-DeckBitLocker {
    try { return "$((Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop).ProtectionStatus)" } catch { return 'unknown' }
}

function Get-DeckCertThumbprints {
    $thumbs = @()
    if (Test-Path -LiteralPath $DeckCertFile) {
        $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($DeckCertFile)
        try { $thumbs += $cert.Thumbprint } finally { $cert.Dispose() }
    }
    if (Test-Path -LiteralPath $DeckBaselineJson) {
        $b = Get-Content -LiteralPath $DeckBaselineJson -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        if ($b.testCertThumbprints) { $thumbs += @($b.testCertThumbprints) }
    }
    $thumbs = @($thumbs | Where-Object { $_ } | Select-Object -Unique)
    if (@($thumbs | Where-Object { $_ -notmatch '^[0-9A-Fa-f]{40}$' }).Count -gt 0 -or $thumbs.Count -gt 1) {
        throw 'Project certificate provenance is inconsistent; no certificate may be removed automatically.'
    }
    return $thumbs
}

function Get-DeckCertPresence {
    $thumbs = @(Get-DeckCertThumbprints)
    $found = @()
    if ($thumbs.Count -eq 0) { return $found }
    foreach ($store in 'Root', 'TrustedPublisher') {
        $certs = @(Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction Stop)
        foreach ($c in $certs) {
            if ($thumbs -contains $c.Thumbprint) {
                $found += [pscustomobject]@{ Store = $store; Thumbprint = $c.Thumbprint; Subject = $c.Subject }
            }
        }
    }
    return $found
}

<#
    Get-DeckChecks - the single definition of "is this machine back to stock?"

    Each check carries Pass (or $null when it could not be determined) plus the evidence file the
    expectation came from, so a report can never assert something it did not actually verify.
#>
function Get-DeckChecks {
    $checks = @()

    $stagedError = $null
    $ours = @()
    try { $ours = @(Get-DeckStagedPackages | Where-Object { $_.IsOurs }) }
    catch { $stagedError = $_.Exception.Message }
    $checks += [pscustomobject]@{
        Name = 'No DeckBtUsb/isotest packages staged'
        Pass = if ($stagedError) { $null } else { $ours.Count -eq 0 }
        Detail = if ($stagedError) { $stagedError } elseif ($ours.Count) { ($ours | ForEach-Object { "$($_.Published)[$($_.OriginalName)]" }) -join ' ' } else { 'none' }
        Source = 'pnputil /enum-drivers vs recovery/drivers-before.txt'
    }

    $svcLeft = @($DeckServices | Where-Object { Test-Path -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\$_" })
    $checks += [pscustomobject]@{
        Name = 'Project services removed'
        Pass = ($svcLeft.Count -eq 0)
        Detail = if ($svcLeft.Count) { $svcLeft -join ', ' } else { 'none of ' + ($DeckServices -join '/') }
        Source = 'HKLM services'
    }

    $devLeft = @($DeckDevnodes | Where-Object { Get-DeckDevice $_ })
    $checks += [pscustomobject]@{
        Name = 'Project devnodes removed'
        Pass = ($devLeft.Count -eq 0)
        Detail = if ($devLeft.Count) { $devLeft -join ', ' } else { 'none' }
        Source = 'PnP present devices'
    }

    $radioError = $null
    $radio = $null
    try { $radio = Assert-DeckHealthyVendorRadio } catch { $radioError = $_.Exception.Message }
    $checks += [pscustomobject]@{
        Name = "$RadioAcpiId owned by $VendorService"
        Pass = if ($radioError) { if ($radioError -like 'REFUSAL:*') { $false } else { $null } } else { $true }
        Detail = if ($radioError) { $radioError } else { "$($radio.Radio) service=$($radio.RadioService) status=$($radio.RadioStatus)" }
        Source = 'recovery/bt-before.txt'
    }

    $checks += [pscustomobject]@{
        Name = 'Bluetooth radio child present'
        Pass = if ($radioError) { if ($radioError -like 'REFUSAL:*') { $false } else { $null } } else { $true }
        Detail = if ($radioError) { $radioError } else { "$($radio.Child) status=$($radio.ChildStatus) service=$($radio.ChildService)" }
        Source = 'recovery/bt-before.txt'
    }

    $paired = Get-DeckPairedDevices
    $expected = $null
    if (Test-Path -LiteralPath $DeckBaselineJson) {
        try { $expected = @((Get-Content -LiteralPath $DeckBaselineJson -Raw | ConvertFrom-Json).pairedDevices).Count } catch { }
    }
    $checks += [pscustomobject]@{
        Name = 'Paired devices intact'
        Pass = if ($null -eq $paired) { $null } elseif ($null -eq $expected) { $null } else { $paired.Count -ge $expected }
        Detail = "present=$(if ($null -ne $paired) { $paired.Count } else { 'unknown' }) baseline=$(if ($null -ne $expected) { $expected } else { 'no baseline' })"
        Source = 'recovery/baseline/baseline.json'
    }

    $certError = $null
    $certs = @()
    $thumbs = @()
    try {
        $thumbs = @(Get-DeckCertThumbprints)
        if ($thumbs.Count) { $certs = @(Get-DeckCertPresence) }
    } catch { $certError = $_.Exception.Message }
    $checks += [pscustomobject]@{
        Name = 'Test certificate removed'
        Pass = if ($certError -or $thumbs.Count -eq 0) { $null } else { $certs.Count -eq 0 }
        Detail = if ($certError) { $certError } elseif ($thumbs.Count -eq 0) { 'project thumbprint unavailable; certificate presence unresolved' } elseif ($certs.Count) { ($certs | ForEach-Object { "$($_.Store):$($_.Thumbprint)" }) -join ' ' } else { 'absent from Root and TrustedPublisher' }
        Source = 'recovery/DeckBtUsbTestCert.cer'
    }

    # One bcdedit read backs both boot-flag checks below.
    $boot = Get-DeckBootFlags
    $checks += [pscustomobject]@{
        Name = 'Test signing off'
        Pass = if (-not $boot.Readable -or $boot.TestSigning -eq 'unknown') { $null } else { $boot.TestSigning -eq 'off' }
        Detail = "testsigning=$($boot.TestSigning)"
        Source = 'bcdedit /enum {current}'
    }

    $checks += [pscustomobject]@{
        Name = 'Debug boot flags cleared'
        Pass = if (-not $boot.Readable) { $null } else { $boot.Leftovers.Count -eq 0 }
        Detail = $boot.Detail
        Source = 'bcdedit /enum {current} vs debug boot flags'
    }

    return $checks
}
