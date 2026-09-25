<#
    service-selftest.ps1 - host test for service record reads, remnant cleanup, and physical radio guard.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'deck-state.ps1')

$script:passed = 0
$script:failures = [System.Collections.Generic.List[string]]::new()

function Test-Case([string] $Name, [scriptblock] $Body) {
    try {
        & $Body
        $script:passed++
        Write-Host ("  [pass] {0}" -f $Name) -ForegroundColor DarkGreen
    } catch {
        $script:failures.Add("$Name :: $($_.Exception.Message)")
        Write-Host ("  [FAIL] {0}: {1}" -f $Name, $_.Exception.Message) -ForegroundColor Red
    }
}

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-Equal($Expected, $Actual, [string] $Message) {
    if ($Expected -ne $Actual) { throw "$Message (expected '$Expected', got '$Actual')" }
}

function Assert-Throws([scriptblock] $Body, [string[]] $Contains, [string] $Message) {
    $thrown = $null
    try { & $Body } catch { $thrown = $_.Exception.Message }
    if ($null -eq $thrown) { throw "${Message}: nothing was thrown" }
    foreach ($needle in $Contains) {
        if ($thrown -notlike "*$needle*") { throw "${Message}: refusal text lacks '$needle': $thrown" }
    }
}

# ---------------------------------------------------------------- record reads (real registry, HKCU)

$testRoot = Join-Path 'HKCU:\Software\DeckBtUsbSelfTest' ([guid]::NewGuid().ToString('N'))
$testServicesRoot = Join-Path $testRoot 'Services'
$shippingImagePath = '\SystemRoot\System32\DriverStore\FileRepository\deckbtusb.inf_amd64_6ba6f14dc81e7aee\deckbtusb.sys'

function Reset-TestHive {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
    [void](New-Item -Path $testServicesRoot -Force)
}

function New-TestService([string] $Name, [int] $Type, [string] $ImagePath, [switch] $WithBreadcrumbs) {
    $key = Join-Path $testServicesRoot $Name
    [void](New-Item -Path $key -Force)
    Set-ItemProperty -LiteralPath $key -Name Type -Value $Type -Type DWord
    Set-ItemProperty -LiteralPath $key -Name Start -Value 4 -Type DWord
    if ($null -ne $ImagePath) { Set-ItemProperty -LiteralPath $key -Name ImagePath -Value $ImagePath -Type ExpandString }
    if ($WithBreadcrumbs) {
        $params = Join-Path $key 'Parameters'
        [void](New-Item -Path $params -Force)
        Set-ItemProperty -LiteralPath $params -Name UartProbeRan -Value 1 -Type DWord
        Set-ItemProperty -LiteralPath $params -Name UartSerialOpened -Value 0 -Type DWord
        Set-ItemProperty -LiteralPath $params -Name UartFailurePhase -Value 'MissingSerialResource' -Type String
        Set-ItemProperty -LiteralPath $params -Name Backend -Value 1 -Type DWord
    }
    return $key
}

Write-Host 'Service record reads:'

Test-Case 'absent service key reads as $null, not as an empty object' {
    Reset-TestHive
    $record = Get-DeckServiceRecord 'DeckBtUsb' $testServicesRoot
    Assert-True ($null -eq $record) 'an absent key must be $null'
    Assert-Equal $false ([bool]$record) 'an absent key must also be falsy'
}

Test-Case 'present service reports type, start, image path and Uart breadcrumbs' {
    Reset-TestHive
    [void](New-TestService 'DeckBtUsb' 1 $shippingImagePath -WithBreadcrumbs)
    $record = Get-DeckServiceRecord 'DeckBtUsb' $testServicesRoot
    Assert-True ($null -ne $record) 'a present key must produce a record'
    Assert-Equal 'DeckBtUsb' $record.Name 'Name'
    Assert-Equal 1 $record.Type 'Type'
    Assert-Equal 4 $record.Start 'Start'
    Assert-Equal $shippingImagePath $record.ImagePath 'ImagePath'
    Assert-Equal $true $record.HasParameters 'HasParameters'
    $names = @($record.UartValueNames)
    Assert-Equal 3 $names.Count 'only Uart* value names belong in the record'
    Assert-Equal 'UartFailurePhase,UartProbeRan,UartSerialOpened' ($names -join ',') 'Uart value names must be sorted'
}

Test-Case 'service without Parameters reports no breadcrumbs' {
    Reset-TestHive
    [void](New-TestService 'DeckBtUsb' 1 $shippingImagePath)
    $record = Get-DeckServiceRecord 'DeckBtUsb' $testServicesRoot
    Assert-Equal $false $record.HasParameters 'HasParameters must be false without the subkey'
    Assert-Equal 0 @($record.UartValueNames).Count 'UartValueNames must be an empty array, not $null'
}

Test-Case 'an unreadable key throws instead of reporting absence' {
    Reset-TestHive
    [void](New-TestService 'DeckBtUsb' 1 $shippingImagePath -WithBreadcrumbs)
    function Get-ItemProperty {
        [CmdletBinding()]
        param([string] $LiteralPath, [string] $Path)
        Write-Error 'Requested registry access is not allowed.'
    }
    try {
        Assert-Throws { Get-DeckServiceRecord 'DeckBtUsb' $testServicesRoot } @('registry access') `
            'a failed read must never be laundered into "service absent"'
    } finally { Remove-Item function:Get-ItemProperty -Force }
}

$script:runnerCalls = [System.Collections.Generic.List[string]]::new()
function New-Runner([int] $Code, [string] $Output) {
    $log = $script:runnerCalls
    return {
        param($Exe, $Arguments)
        $log.Add(("{0} {1}" -f $Exe, ($Arguments -join ' ')))
        return [pscustomobject]@{ Code = $Code; Output = $Output }
    }.GetNewClosure()
}

# ---------------------------------------------------------------- valueless service remnants

Write-Host 'Owned service remnants:'

Test-Case 'a valueless owned key with only Parameters is a remnant' {
    $r = [pscustomobject]@{ Name = 'DeckBtUsb'; Type = $null; Start = $null; ImagePath = $null
                            HasParameters = $true; UartValueNames = @() }
    Assert-Equal $true (Test-DeckOwnedServiceRemnant $r) 'the observed live shape must be recognised'
}

Test-Case 'other owned service can also be a remnant' {
    $r = [pscustomobject]@{ Name = 'DeckBtFlt'; Type = $null; Start = $null; ImagePath = $null
                            HasParameters = $true; UartValueNames = @() }
    Assert-Equal $true (Test-DeckOwnedServiceRemnant $r) 'every owned service name qualifies'
}

Test-Case 'a configured service is never a remnant' {
    foreach ($case in @(
        @{ Type = 1;     ImagePath = $null },
        @{ Type = $null; ImagePath = '\SystemRoot\System32\drivers\deckbtusb.sys' },
        @{ Type = 1;     ImagePath = '\SystemRoot\System32\drivers\deckbtusb.sys' }
    )) {
        $r = [pscustomobject]@{ Name = 'DeckBtUsb'; Type = $case.Type; Start = 3
                                ImagePath = $case.ImagePath; HasParameters = $true; UartValueNames = @() }
        Assert-Equal $false (Test-DeckOwnedServiceRemnant $r) 'anything with real configuration must be left alone'
    }
}

Test-Case 'a foreign name is never a remnant' {
    foreach ($name in @('QcBluetooth', 'BthMini', 'DeckBtUsbX', 'Tcpip')) {
        $r = [pscustomobject]@{ Name = $name; Type = $null; Start = $null; ImagePath = $null
                                HasParameters = $true; UartValueNames = @() }
        Assert-Equal $false (Test-DeckOwnedServiceRemnant $r) "'$name' is not an owned service"
    }
    Assert-Equal $false (Test-DeckOwnedServiceRemnant $null) 'an absent record is not a remnant'
}

Test-Case 'a remnant is removed and its breadcrumbs with it' {
    $script:radioScenario = 'Healthy'
    Reset-TestHive
    $script:runnerCalls.Clear()
    $key = Join-Path $testServicesRoot 'DeckBtUsb'
    [void](New-Item -Path (Join-Path $key 'Parameters') -Force)
    $sibling = New-TestService 'QcBluetooth' 1 'C:\Windows\System32\drivers\qcbtuart.sys'
    $r = Remove-DeckOwnedServiceRemnant 'DeckBtUsb' $testServicesRoot (New-Runner 0 '')
    Assert-Equal $true $r.Removed 'the remnant is gone'
    Assert-Equal $false (Test-Path -LiteralPath $key) 'the service key is deleted'
    Assert-Equal $true (Test-Path -LiteralPath $sibling) 'a real neighbouring service is untouched'
    Assert-Equal 1 $script:runnerCalls.Count 'exactly one service delete'
    Assert-Equal 'sc.exe delete DeckBtUsb' $script:runnerCalls[0] 'the delete names the exact service'
}

Test-Case 'a configured owned service is refused, not deleted' {
    $script:radioScenario = 'Healthy'
    Reset-TestHive
    $script:runnerCalls.Clear()
    $key = New-TestService 'DeckBtUsb' 1 '\SystemRoot\System32\DriverStore\FileRepository\deckbtusb.inf_amd64_abc123\deckbtusb.sys' -WithBreadcrumbs
    Assert-Throws { Remove-DeckOwnedServiceRemnant 'DeckBtUsb' $testServicesRoot (New-Runner 0 '') } `
        @('not a remnant') 'a real installed service must never be deleted by remnant cleanup'
    Assert-Equal $true (Test-Path -LiteralPath $key) 'the configured service survives'
    Assert-Equal 0 $script:runnerCalls.Count 'no delete is issued'
}

Test-Case 'remnant removal is idempotent' {
    Reset-TestHive
    $script:runnerCalls.Clear()
    $r = Remove-DeckOwnedServiceRemnant 'DeckBtUsb' $testServicesRoot (New-Runner 0 '')
    Assert-Equal $true $r.Removed 'absent is already the goal state'
    Assert-Equal 0 $script:runnerCalls.Count 'nothing is deleted when nothing exists'
}

if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }

# ---------------------------------------------------------------- physical radio guard

Write-Host 'Physical radio guard (shadowed PnP cmdlets, no real device access):'

$script:radioScenario = 'Healthy'
function Get-PnpDevice {
    [CmdletBinding()]
    param([string] $InstanceId, [switch] $PresentOnly)
    switch ($script:radioScenario) {
        'Healthy' {
            if ($InstanceId -like 'ACPI\QCOM2066*') { return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&0'; Status = 'OK'; Problem = 'CM_PROB_NONE' } }
            if ($InstanceId -like 'QCA_SHB\UART_H4*') { return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\6&1'; Status = 'OK'; Problem = 'CM_PROB_NONE' } }
        }
        'ChildMissing' {
            if ($InstanceId -like 'ACPI\QCOM2066*') { return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&0'; Status = 'OK'; Problem = 'CM_PROB_NONE' } }
            Write-Error 'No matching child' -Category ObjectNotFound -ErrorId CmdletizationQuery_NotFound_InstanceId
            return
        }
        'RadioAmbiguous' {
            if ($InstanceId -like 'ACPI\QCOM2066*') {
                return @(
                    [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&0'; Status = 'OK'; Problem = 'CM_PROB_NONE' },
                    [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&1'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
                )
            }
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\6&1'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        'ChildDegraded' {
            if ($InstanceId -like 'ACPI\QCOM2066*') { return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&0'; Status = 'OK'; Problem = 'CM_PROB_NONE' } }
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\6&1'; Status = 'Error'; Problem = 'CM_PROB_FAILED_START' }
        }
        'Claimed' {
            if ($InstanceId -like 'ACPI\QCOM2066*') { return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\3&185af18b&0'; Status = 'OK'; Problem = 'CM_PROB_NONE' } }
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\6&1'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        'QueryFailure' { throw 'Simulated PnP access failure' }
    }
}
function Get-PnpDeviceProperty {
    [CmdletBinding()]
    param([string] $InstanceId, [string] $KeyName)
    if ($InstanceId -like 'ACPI\QCOM2066*') {
        $svc = if ($script:radioScenario -eq 'Claimed') { 'DeckBtUsb' } else { 'QcBluetooth' }
        return [pscustomobject]@{ Data = $svc }
    }
    return [pscustomobject]@{ Data = 'BthMini' }
}

Test-Case 'healthy vendor-owned radio passes and reports both nodes' {
    $script:radioScenario = 'Healthy'
    $state = Assert-DeckHealthyVendorRadio
    Assert-Equal 'QcBluetooth' $state.RadioService 'RadioService'
    Assert-Equal 'OK' $state.RadioStatus 'RadioStatus'
    Assert-Equal 'BthMini' $state.ChildService 'ChildService'
    Assert-Equal 'OK' $state.ChildStatus 'ChildStatus'
    Assert-True ("$($state.Radio)" -like 'ACPI\QCOM2066*') 'Radio instance id'
    Assert-True ("$($state.Child)" -like 'QCA_SHB\UART_H4*') 'Child instance id'
}

Test-Case 'missing child refuses' {
    $script:radioScenario = 'ChildMissing'
    Assert-Throws { Assert-DeckHealthyVendorRadio } @('REFUSAL') 'a missing Bluetooth child must refuse'
}

Test-Case 'ambiguous radio identity refuses' {
    $script:radioScenario = 'RadioAmbiguous'
    Assert-Throws { Assert-DeckHealthyVendorRadio } @('REFUSAL') 'two present radios must refuse'
}

Test-Case 'unhealthy child refuses' {
    $script:radioScenario = 'ChildDegraded'
    Assert-Throws { Assert-DeckHealthyVendorRadio } @('REFUSAL') 'a failed child must refuse'
}

Test-Case 'radio claimed by the DeckBtUsb driver refuses' {
    $script:radioScenario = 'Claimed'
    Assert-Throws { Assert-DeckHealthyVendorRadio } @('REFUSAL') 'a non-vendor owner must refuse'
}

Test-Case 'a failed PnP query propagates instead of passing' {
    $script:radioScenario = 'QueryFailure'
    Assert-Throws { Assert-DeckHealthyVendorRadio } @('Simulated PnP access failure') 'an unreadable PnP state must never look healthy'
}

# ---------------------------------------------------------------- verdict

Write-Host ''
if ($script:failures.Count -eq 0) {
    Write-Host ("SERVICE SELFTEST PASSED: all {0} checks passed" -f $script:passed) -ForegroundColor Green
    exit 0
}
Write-Host ("SERVICE SELFTEST FAILED: {0} of {1} checks" -f $script:failures.Count, ($script:failures.Count + $script:passed)) -ForegroundColor Red
foreach ($f in $script:failures) { Write-Host "  - $f" -ForegroundColor Red }
exit 1
