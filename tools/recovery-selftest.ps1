<# Isolated recovery regressions. Production function bodies run against in-memory machine state. #>
[CmdletBinding()]
param([string] $SourceDir)
$ErrorActionPreference = 'Stop'
$SourceDir = if ($SourceDir) { [IO.Path]::GetFullPath($SourceDir) } else { $PSScriptRoot }
$failed = 0
$passed = 0
function Get-TestFunctions([string] $File) {
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceDir $File), [ref]$null, [ref]$errors)
    if ($errors) { throw ($errors | Out-String) }
    return [scriptblock]::Create((@($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] } | ForEach-Object { $_.Extent.Text }) -join "`n"))
}
function Check([bool] $Condition, [string] $Message) { if (-not $Condition) { throw $Message } }
function Case([string] $Name, [scriptblock] $Body) {
    try { & $Body; $script:passed++; Write-Host "  [pass] $Name" }
    catch { $script:failed++; Write-Host "  [FAIL] ${Name}: $($_.Exception.Message)" }
}
function Must-Fail([scriptblock] $Body) {
    $caught = $false
    try { & $Body | Out-Null } catch { $caught = $true }
    Check $caught 'Operation unexpectedly succeeded'
}

Case 'package query failure is not an empty DriverStore' {
    . (Get-TestFunctions 'deck-state.ps1')
    function Invoke-DeckNative { [pscustomobject]@{ Code = 5; Output = 'Access denied' } }
    Must-Fail { Get-DeckStagedPackages }
}
Case 'unknown package output fails closed' {
    . (Get-TestFunctions 'deck-state.ps1')
    Must-Fail { Get-DeckPackageList 'Unrecognized package inventory' }
    Must-Fail { Get-DeckPackageList '' }
}
Case 'package identity requires provider as well as INF name' {
    . (Get-TestFunctions 'deck-state.ps1')
    $DeckInfNames = @('deckbtusb.inf'); $DeckProvider = 'DeckBtUsb'
    $text = @'
Microsoft PnP Utility

Published Name: oem10.inf
Original Name: deckbtusb.inf
Provider Name: Unrelated
Driver Version: 09/01/2026 1.0.0.0
Attributes: Universal
            Attested

Published Name: oem11.inf
Original Name: deckbtusb.inf
Provider Name: DeckBtUsb
Driver Version: 09/02/2026 1.0.0.0
'@
    $packages = @(Get-DeckPackageList $text)
    Check ($packages.Count -eq 2 -and -not $packages[0].IsOurs -and $packages[1].IsOurs) 'Package provenance was lost'
}
Case 'missing certificate provenance never enumerates or removes trust' {
    . (Get-TestFunctions 'uninstall.ps1')
    $script:exitCode = 0; $script:steps = @()
    function Get-OurCertThumbprints { @() }
    function Get-DeckCertPresence { throw 'Certificate store must not be queried' }
    function Remove-Item { throw 'Certificate store must not be modified' }
    Step-RemoveCert
    Check ($script:exitCode -ne 0) 'Missing identity must be unresolved, not clean'
}
Case 'certificate lookup ignores unrelated WDK subjects' {
    . (Get-TestFunctions 'deck-state.ps1')
    $ours = 'A' * 40; $foreign = 'B' * 40
    function Get-DeckCertThumbprints { $ours }
    function Get-ChildItem {
        [pscustomobject]@{ Thumbprint = $ours; Subject = 'CN=WDKTestCert Project' }
        [pscustomobject]@{ Thumbprint = $foreign; Subject = 'CN=WDKTestCert Other' }
    }
    $found = @(Get-DeckCertPresence)
    Check ($found.Count -eq 2 -and @($found | Where-Object Thumbprint -ne $ours).Count -eq 0) 'Unrelated certificate selected'
}
Case 'certificate removal touches only the selected exact thumbprint' {
    . (Get-TestFunctions 'uninstall.ps1')
    $ours = 'A' * 40
    $state = @{ Present = $true; Removed = @() }
    $script:exitCode = 0; $script:steps = @(); $DryRun = $false
    function Get-OurCertThumbprints { $ours }
    function Get-DeckCertPresence {
        if ($state.Present) { [pscustomobject]@{ Store = 'Root'; Thumbprint = $ours } }
    }
    function Remove-Item { param($LiteralPath, [switch]$Force, $ErrorAction)
        $state.Removed += $LiteralPath; $state.Present = $false
    }
    Step-RemoveCert
    Check ($state.Removed.Count -eq 1 -and $state.Removed[0] -eq "Cert:\LocalMachine\Root\$ours") 'Wrong certificate removal target'
    Check ($script:exitCode -eq 0) 'Exact certificate removal failed'
}
foreach ($failure in @('Step-RestoreRadio', 'Step-RemovePackages', 'Step-RemoveServices', 'Step-RemoveCert')) {
    Case "uninstall halts after $failure" {
        . (Get-TestFunctions 'uninstall.ps1')
        $script:exitCode = 0; $script:steps = @()
        $state = @{ Calls = [Collections.Generic.List[string]]::new() }
        foreach ($name in @('Step-Disarm', 'Step-RemoveDevnodes', 'Step-RestoreRadio', 'Step-RemovePackages', 'Step-RemoveServices', 'Step-RemoveCert', 'Step-TestSigningOff', 'Step-ClearBootFlags', 'Step-CodeIntegrity')) {
            Set-Item -LiteralPath "Function:$name" -Value ([scriptblock]::Create("[void]`$state.Calls.Add('$name'); if ('$name' -eq `$failure) { Add-Step '$name' 'FAIL' 'injected failure' }"))
        }
        Invoke-UninstallSteps
        Check ($script:exitCode -ne 0) 'Failure status was lost'
        Check ($state.Calls[$state.Calls.Count - 1] -eq $failure) 'Cleanup continued after failure'
        Check (-not $state.Calls.Contains('Step-TestSigningOff')) 'Signing changed after unsafe cleanup'
    }
}
Case 'uninstall devnode reboot requirement stops cleanup' {
    . (Get-TestFunctions 'uninstall.ps1')
    $script:exitCode = 0; $script:steps = @(); $DryRun = $false
    $OurDevnodes = @('ROOT\DEVGEN\DECKBTUSB')
    function Get-DeviceInfo { [pscustomobject]@{ InstanceId = $OurDevnodes[0] } }
    function Invoke-Native { [pscustomobject]@{ Code = 3010; Output = 'Restart required' } }
    function Start-Sleep { throw 'No post-failure device checks are permitted' }
    Step-RemoveDevnodes
    Check ($script:exitCode -ne 0) 'Reboot-required removal was reported successful'
}
Case 'uninstall dry run does not require planned changes to be applied' {
    . (Get-TestFunctions 'uninstall.ps1')
    $script:exitCode = 0; $script:steps = @(); $DryRun = $true
    function Get-StagedPackages { throw 'Package removal has only been planned' }
    function Assert-DeckHealthyVendorRadio { throw 'Vendor restore has only been planned' }
    function Invoke-Native { throw 'Dry run must not mutate the machine' }
    Step-RemoveServices
    Check ($script:exitCode -eq 0 -and $script:steps[0].State -eq 'PLAN') 'Dry run falsely failed'
}
Case 'unhealthy vendor radio and failed inventory cannot produce clean checks' {
    . (Get-TestFunctions 'deck-state.ps1')
    $RadioAcpiId = 'ACPI\QCOM2066'; $VendorService = 'QcBluetooth'
    $DeckServices = @(); $DeckDevnodes = @(); $DeckBaselineJson = 'missing-baseline'
    function Get-DeckStagedPackages { throw 'Inventory unavailable' }
    function Assert-DeckHealthyVendorRadio { throw 'REFUSAL: child is unhealthy' }
    function Get-DeckPairedDevices { @() }
    function Get-DeckCertThumbprints { @() }
    function Test-Path { $false }
    function Get-DeckBootFlags { [pscustomobject]@{ Readable = $true; TestSigning = 'off'; Leftovers = @(); Detail = 'off' } }
    $checks = @(Get-DeckChecks)
    $inventory = $checks | Where-Object Name -eq 'No DeckBtUsb/isotest packages staged'
    $radio = $checks | Where-Object Name -eq 'ACPI\QCOM2066 owned by QcBluetooth'
    $child = $checks | Where-Object Name -eq 'Bluetooth radio child present'
    Check ($null -eq $inventory.Pass) 'Failed inventory was reported clean'
    Check ($radio.Pass -eq $false -and $child.Pass -eq $false) 'Unhealthy radio was reported clean'
}
Case 'repeat stub Prepare preserves existing recovery snapshots' {
    . (Get-TestFunctions 'stub-install.ps1')
    $StateDir = 'TestDrive:\recovery'
    function Join-Path { param($Path, $ChildPath) "$Path\$ChildPath" }
    function New-Item { }
    function Test-Path { $true }
    function Invoke-Native { throw 'Existing snapshots must not be recaptured' }
    function Set-Content { throw 'Existing snapshots must not be overwritten' }
    function Enable-ComputerRestore { }
    function Checkpoint-Computer { }
    Save-Snapshots
}
Case 'stub snapshot capture failure is reported before writing output' {
    . (Get-TestFunctions 'stub-install.ps1')
    $StateDir = 'TestDrive:\recovery'
    $state = @{ Wrote = $false }
    function Join-Path { param($Path, $ChildPath) "$Path\$ChildPath" }
    function New-Item { }
    function Test-Path { param($LiteralPath) $LiteralPath -like '*bcd-backup.bcd' }
    function Invoke-Native { 5 }
    function Set-Content { $state.Wrote = $true }
    Must-Fail { Save-Snapshots }
    Check (-not $state.Wrote) 'Failed snapshot was persisted'
}
Case 'stub restore reports missing radio as failure' {
    . (Get-TestFunctions 'stub-install.ps1')
    function Get-RealRadio { @() }
    function Get-PnpDevice { @() }
    Must-Fail { Enable-RealRadio }
}
Case 'stub removal failure does not continue deleting packages' {
    . (Get-TestFunctions 'stub-install.ps1')
    function Get-DeckDevice { [pscustomobject]@{ InstanceId = 'ROOT\DEVGEN\DECKBTUSB' } }
    function Invoke-Native { 3010 }
    $state = @{ Continued = $false }
    function Get-DeckPublishedName { $state.Continued = $true; @() }
    Must-Fail { Uninstall-Package }
    Check (-not $state.Continued) 'Package queries continued after failed device removal'
}

foreach ($code in @(5, 3010)) {
    Case "Stop native exit $code is not success" {
        . (Get-TestFunctions 'restore-radio.ps1')
        function Invoke-RestoreBounded { [pscustomobject]@{ Code = $code; Output = 'native result' } }
        Must-Fail { Invoke-RestorePnp @('/delete-driver', 'oem10.inf', '/uninstall') }
    }
}
foreach ($childExit in @(0, 1)) {
    Case "session child exit $childExit controls subsequent operations" {
        $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceDir 'session.ps1'), [ref]$null, [ref]$null)
        $branch = $ast.Find({ param($node)
            $node -is [Management.Automation.Language.IfStatementAst] -and
            $node.Extent.Text.StartsWith('if ($Identify -or $Probe -or $Bridge -or $Start)')
        }, $true)
        if (-not $branch) { throw 'Session operation branch not found' }
        $state = @{ ChildReturned = $false; PostOperations = 0 }
        $Start = $true; $Identify = $Probe = $Bridge = $false
        $session = 'mock-session'; $exitCode = -1
        function Invoke-Tool {
            param($Script, $Arguments)
            if ($Script -eq 'trust-cert.ps1') { return 0 }
            $state.ChildReturned = $true
            return $childExit
        }
        function Get-VendorRadio { [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\TEST'; Problem = 22 } }
        function Get-DeckPairedDevices { if ($state.ChildReturned) { $state.PostOperations++ }; @() }
        function Get-Service { $state.PostOperations++; [pscustomobject]@{ Status = 'Running' } }
        function Start-Service { $state.PostOperations++ }
        . ([scriptblock]::Create($branch.Extent.Text))
        Check ($exitCode -eq $childExit) 'Child exit status was changed'
        if ($childExit -ne 0) { Check ($state.PostOperations -eq 0) 'Session touched service/devices after child failure' }
        else { Check ($state.PostOperations -gt 0) 'Successful session did not perform final observations' }
    }
}

# Execute the actual top-level Stop flow, replacing only its machine-facing functions.
foreach ($scenario in @('PrepareOnly', 'IdleAfterBoot', 'Active', 'DeleteFailed', 'PendingReboot', 'ReleaseUnknown', 'PackageRemains')) {
    Case "Stop flow: $scenario" {
        . (Get-TestFunctions 'restore-radio.ps1')
        $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceDir 'restore-radio.ps1'), [ref]$null, [ref]$null)
        $flow = @($ast.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] })[-1]
        $state = @{ Calls = [Collections.Generic.List[string]]::new(); Deleted = $false; Enabled = $false; Fresh = $false; Startup = 'Manual'; Exit = -1 }
        $paramsKey = 'HKLM:\mock\Parameters'; $bthStart = $null
        $owner = if ($scenario -eq 'PrepareOnly') { 'QcBluetooth' } else { 'DeckBtUsb' }
        $id = 'ACPI\QCOM2066\TEST'
        function Test-Path { $scenario -ne 'PrepareOnly' }
        function Invoke-RestoreBounded {
            param($Label, $Action, $Arguments, $TimeoutSeconds)
            [void]$state.Calls.Add($Label)
            switch ($Label) {
                'Read radio' { [pscustomobject]@{ InstanceId = $id; Status = 'OK' } }
                'Read radio owner' { $owner }
                'Enumerate DeckBtUsb packages' { if ($scenario -ne 'PrepareOnly') { [pscustomobject]@{ Published = 'oem10.inf' } } }
                'Read live bridge child' { if ($scenario -ne 'IdleAfterBoot') { [pscustomobject]@{ InstanceId = 'USB\VID_0CF3&PID_6390\TEST' } } }
                'Invalidate stale release record' { $state.Fresh = $true }
                'Disarm DeckBtUsb' { }
                'Confirm package retirement' { if ($scenario -eq 'PackageRemains') { [pscustomobject]@{ Published = 'oem10.inf' } } }
                'Read fresh controller handback' { [pscustomobject]@{ UartCompletion = $(if ($scenario -eq 'ReleaseUnknown') { 2 } else { 1 }); UartFailurePhase = 'Stopped'; UartHandbackBaud = 115200; UartHandbackStatus = 0 } }
                'Remove Enabled parameter' { }
                'Read radio state' { [pscustomobject]@{ InstanceId = $id; Problem = $(if ($scenario -eq 'PrepareOnly') { 22 } else { 0 }) } }
                'Verify vendor radio health' { [pscustomobject]@{ Radio = $id; Child = 'QCA_SHB\UART_H4\TEST' } }
                default { throw "Unexpected machine operation: $Label" }
            }
        }
        function Invoke-RestorePnp {
            param($Arguments)
            [void]$state.Calls.Add($Arguments[0])
            if ($Arguments[0] -eq '/delete-driver') {
                if ($scenario -in @('DeleteFailed', 'PendingReboot')) { throw 'Injected deletion failure or pending reboot' }
                $state.Deleted = $true
            }
            if ($Arguments[0] -eq '/enable-device') { $state.Enabled = $true }
        }
        function Get-CimInstance { [pscustomobject]@{ StartMode = 'Manual' } }
        function Set-Service { param($Name, $StartupType) $state.Startup = $StartupType }
        function Stop-BthservBounded { }
        function Get-Service { [pscustomobject]@{ Status = 'Stopped' } }
        function Start-Service { [void]$state.Calls.Add('Start-Service') }
        function Get-DeckPairedDevices { @() }
        function Start-Sleep { }
        # Run the try body without its process-exiting catch. Exceptions are the failure outcome.
        $ok = $true
        $body = [scriptblock]::Create(($flow.Body.Statements | Where-Object { $_ -isnot [Management.Automation.Language.ExitStatementAst] } | ForEach-Object { $_.Extent.Text }) -join "`n")
        try { . $body } catch { $ok = $false }
        if ($scenario -in @('DeleteFailed', 'PendingReboot', 'ReleaseUnknown', 'PackageRemains')) {
            Check (-not $ok) 'Unsafe Stop unexpectedly succeeded'
            Check (-not $state.Calls.Contains('/scan-devices') -and -not $state.Calls.Contains('/enable-device') -and -not $state.Calls.Contains('Start-Service')) 'Unsafe Stop continued with device operations'
        } else {
            Check $ok 'Valid Stop failed'
            if ($scenario -eq 'PrepareOnly') { Check ($state.Enabled -and -not $state.Deleted -and -not $state.Calls.Contains('Disarm DeckBtUsb')) 'Prepare-only Stop touched nonexistent project state' }
            if ($scenario -eq 'IdleAfterBoot') { Check (-not $state.Fresh) 'Idle-after-boot incorrectly required a worker release' }
            if ($scenario -eq 'Active') { Check ($state.Deleted -and $state.Fresh -and $state.Calls.Contains('Read fresh controller handback')) 'Active Stop skipped fresh release verification' }
        }
    }
}

Write-Host "RECOVERY SELFTEST: $passed passed, $failed failed"
if ($failed) { exit 1 }
exit 0
