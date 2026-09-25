<#
    operator-selftest.ps1 - host regression suite for operator flows, physical restore, and bridge scenarios.
    Runs disposable workers with mocked Get-PnpDevice for transport checks, and an in-memory
    AST operator flow harness for operator scenarios.
    Never queries real devices, never alters live registry, and never requires elevation.
#>
[CmdletBinding()]
param(
    [string] $SourceDir
)
$ErrorActionPreference = 'Stop'
$toolsDir = if (-not [string]::IsNullOrWhiteSpace($SourceDir)) { [IO.Path]::GetFullPath($SourceDir) }
            elseif ($PSScriptRoot) { $PSScriptRoot }
            else { '.' }
$deckStateDir = if (Test-Path -LiteralPath (Join-Path $toolsDir 'deck-state.ps1')) { $toolsDir }
                elseif ($PSScriptRoot) { $PSScriptRoot }
                else { '.' }
. (Join-Path $deckStateDir 'deck-state.ps1')

$script:failedScenario = $null
$script:failureError = $null
$script:scenarioCount = 0

function Run-NamedScenario([string] $Name, [scriptblock] $Body) {
    if ($null -ne $script:failedScenario) { return }
    $script:scenarioCount++
    try {
        & $Body
        Write-Host ("  [pass] {0}" -f $Name) -ForegroundColor DarkGreen
    } catch {
        $script:failedScenario = $Name
        $script:failureError = $_.Exception.Message
        Write-Host ("  [FAIL] {0}: {1}" -f $Name, $_.Exception.Message) -ForegroundColor Red
    }
}

# session.ps1 must run exactly one mode per invocation: each mode alone is accepted, none or two refused.
Run-NamedScenario 'session.ps1 accepts exactly one mode' {
    $sessionAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'session.ps1'), [ref]$null, [ref]$null)
    $sessionTry = $sessionAst.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] } | Select-Object -Last 1
    $modeCheck = @($sessionTry.Body.Statements | Select-Object -First 2)
    if ($modeCheck.Count -ne 2 -or $modeCheck[1] -isnot [Management.Automation.Language.IfStatementAst]) { throw 'Cannot locate the mode check at the start of session.ps1.' }
    $check = [scriptblock]::Create(($modeCheck | ForEach-Object { $_.Extent.Text }) -join "`n")
    $modes = 'Prepare', 'Identify', 'Probe', 'Bridge', 'Start', 'Stop', 'Uninstall'
    $cases = @(@{ Set = @(); Accept = $false }, @{ Set = @('Start', 'Stop'); Accept = $false }) +
             @($modes | ForEach-Object { @{ Set = @($_); Accept = $true } })
    foreach ($case in $cases) {
        foreach ($m in $modes) { Set-Variable -Name $m -Value ([switch]($case.Set -contains $m)) }
        $accepted = $true
        try { . $check } catch { $accepted = $false }
        if ($accepted -ne $case.Accept) {
            throw ("Modes [{0}]: expected {1}, got {2}." -f ($case.Set -join ','), $(if ($case.Accept) { 'accepted' } else { 'refused' }), $(if ($accepted) { 'accepted' } else { 'refused' }))
        }
    }
}

Run-NamedScenario 'Full restore executes the requested native operation' {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uninstall.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors) { throw ($parseErrors | Out-String) }
    $native = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-Native'
    }, $true)
    if ($null -eq $native) { throw 'Full restore native runner was not found.' }
    . ([scriptblock]::Create($native.Extent.Text))

    # Harmless real command: without its arguments, where.exe returns a syntax error.
    $result = Invoke-Native (Join-Path $env:SystemRoot 'System32\where.exe') @('/q', 'cmd.exe')
    if ($result.Code -ne 0) {
        throw "Native file lookup failed (exit $($result.Code)); restore must execute the requested operation, not an argumentless command. $($result.Output)"
    }
}

Write-Host 'CLIXML transport and phantom refusal regression:'

Run-NamedScenario 'CLIXML transport & phantom regression' {
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uart-probe.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors) { throw ($parseErrors | Out-String) }

    # Load only helpers, not the operator's top-level executable flow.
    foreach ($definition in $ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Write-Step','Invoke-Bounded')
    }, $true)) {
        $text = $definition.Extent.Text.Replace('$PSScriptRoot', ("'" + $deckStateDir.Replace("'", "''") + "'"))
        . ([scriptblock]::Create($text))
    }
    $PnpTimeoutSeconds = 10
    $script:wedged = $false

    # An empty success stream must not cross Export-Clixml as a truthy empty PSCustomObject;
    # the root guard would then refuse a device that does not exist.
    $empty = Invoke-Bounded 'Regression: absent success stream' { }
    if ($null -ne $empty -or [bool]$empty) { throw 'REGRESSION: absent worker result became a truthy phantom.' }
    $number = Invoke-Bounded 'Regression: scalar success stream' { 42 }
    if ($number -ne 42) { throw 'Scalar worker result was lost.' }

    $lookup = {
        param($Scenario)
        function Get-PnpDevice {
            [CmdletBinding()]
            param([string] $InstanceId)
            if ($InstanceId -cne 'ROOT\DEVGEN\DECKBTUSB') { throw 'Unexpected query: regression must target only the exact root.' }
            switch ($Scenario) {
                'Absent' {
                    Write-Error 'No matching root' -Category ObjectNotFound -ErrorId CmdletizationQuery_NotFound_InstanceId
                    return
                }
                'Phantom' { [pscustomobject]@{ InstanceId = $InstanceId; Status = 'Unknown'; Problem = 'CM_PROB_PHANTOM' } }
                'Ambiguous' {
                    [pscustomobject]@{ InstanceId = $InstanceId; Status = 'Unknown' }
                    [pscustomobject]@{ InstanceId = $InstanceId; Status = 'Unknown' }
                }
                'QueryFailure' { throw 'Simulated access failure' }
            }
        }
        Get-DeckExactRoot
    }
    $absent = Invoke-Bounded 'Regression: exact root absent' $lookup @('Absent')
    if ($null -ne $absent -or [bool]$absent) { throw 'Absent exact root became present after CLIXML transport.' }
    $phantom = Invoke-Bounded 'Regression: genuine phantom' $lookup @('Phantom')
    if (-not $phantom -or $phantom.InstanceId -cne 'ROOT\DEVGEN\DECKBTUSB' -or $phantom.Problem -ne 'CM_PROB_PHANTOM') {
        throw 'A genuine phantom was lost or its identity changed; the safety refusal must remain.'
    }
    foreach ($scenario in 'Ambiguous','QueryFailure') {
        $refused = $false
        try { $null = Invoke-Bounded "Regression: $scenario" $lookup @($scenario) }
        catch {
            $expected = if ($scenario -eq 'Ambiguous') { 'identity is ambiguous' } else { 'Simulated access failure' }
            if ($_.Exception.Message -notlike "*$expected*") { throw }
            $refused = $true
        }
        if (-not $refused) { throw "$scenario incorrectly became an absent root." }
    }
}


$tokens = $null
$parseErrors = $null
$probeAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uart-probe.ps1'), [ref]$tokens, [ref]$parseErrors)
$nativeHelper = $probeAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-Pnp'
}, $true)[0]
. ([scriptblock]::Create($nativeHelper.Extent.Text))

$script:capturedHost = [System.Collections.Generic.List[string]]::new()
function Write-Host {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromPipeline = $true)]
        [object] $Object,
        [switch] $NoNewline,
        [ConsoleColor] $ForegroundColor,
        [ConsoleColor] $BackgroundColor,
        [object] $Separator
    )
    $script:capturedHost.Add("$Object")
    Microsoft.PowerShell.Utility\Write-Host @PSBoundParameters
}

function Write-Bad([string] $Message) { Write-Host $Message }

$script:showRecoveryCount = 0
function Show-Recovery {
    $script:showRecoveryCount++
}

function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments = @(), [int] $TimeoutSeconds = 10) {
    if ($Label -in @('Clear stale results and arm UartProbe','Clear UartProbe',
                     'Clear stale results and arm UartIdentify','Clear UartIdentify',
                     'Clear stale results and arm Enabled','Set Enabled=0',
                     'Remove Enabled parameter','Restart bthserv')) {
        if ($Label -eq 'Clear stale results and arm Enabled') { if ($script:mockRegistry) { $script:mockRegistry['Enabled'] = 1 } }
        if ($Label -eq 'Set Enabled=0') { if ($script:mockRegistry) { $script:mockRegistry['Enabled'] = 0 } }
        if ($Label -eq 'Remove Enabled parameter') { if ($script:mockRegistry) { [void]$script:mockRegistry.Remove('Enabled') } }
        if ($Label -eq 'Restart bthserv') { $script:trace.Add('restart-bthserv') }
        return
    }
    if ($Label -eq 'Query and stop bthserv') {
        $script:trace.Add('stop-bthserv')
        return $script:bthWasRunning
    }
    if ($Label -eq 'Query Bluetooth class devices') {
        return @([pscustomobject]@{ InstanceId = 'BTH\MS_BTHBRB\0'; Status = 'OK'; FriendlyName = 'Microsoft Bluetooth Enumerator' })
    }
    if ($Label -eq 'Query USB radio device properties') {
        return @([pscustomobject]@{ KeyName = '{a8b865dd-2e3d-4094-ad97-e593a70c75d6} 2'; Data = '01-02-03' })
    }
    if ($Label -eq 'Query Microsoft Bluetooth enumerators') {
        if ($script:mockEnumerators) { return $script:mockEnumerators }
        return [pscustomobject]@{ BrbPresent = $true; BrbStatus = 'OK'; LePresent = $true; LeStatus = 'OK' }
    }
    if ($Label -eq 'Verify vendor radio health') {
        $script:trace.Add('verify-radio-health')
        return & $Action @Arguments
    }
    if ($Label -eq 'Enumerate staged packages') {
        return & $Action @Arguments
    }
    if ($Label -like 'Read device *') {
        $script:trace.Add('poll-target')
        return & $Action @Arguments
    }
    if ($Label -notlike 'pnputil *') { throw "Unexpected worker: $Label" }
    $native = @($Arguments[0])
    switch ($native[0]) {
        '/add-driver' {
            if ($native -contains '/install') {
                $script:trace.Add('install')
                $script:servicePresent = $true
                if ($native[1] -like '*qcbtuart*') { $script:restored = $true }
            } else {
                $script:trace.Add('stage')
                $script:packagePresent = $true
            }
        }
        '/remove-device' {
            $script:trace.Add('remove-root')
            $script:rootPresent = $false
        }
        '/delete-driver' {
            $script:trace.Add('delete-package')
            if ($null -ne $script:deletedPackages) { $script:deletedPackages.Add($native[1]) }
            $script:packagePresent = $false
            if ($script:uninstallRequiresReboot) {
                return [pscustomobject]@{ Code = 3010; Output = 'Reboot required' }
            }
        }
        '/enable-device' {
            $script:trace.Add('enable-device')
        }
        '/restart-device' {
            $script:trace.Add('restart-device')
        }
        '/scan-devices' {
            $script:trace.Add('scan-devices')
        }
        '/disable-device' {
            $script:trace.Add("disable-device $($native[1])")
            if ($script:disableRequiresReboot) {
                return [pscustomobject]@{ Code = 3010; Output = 'Reboot required' }
            }
        }
        default { throw "Unexpected native operation: $Label" }
    }
    return [pscustomobject]@{ Code = 0; Output = '' }
}
Write-Host 'Identify consumer result classification scenarios (AST-extracted flow against in-memory mock):'

$identifyConsumerCases = @(
    @{
        Name             = 'uart-identify.ps1 StaleBaudWithFailure'
        ExpectedExit     = 1
        IsSuccess        = $false
        RequireInVerdict = @('FAIL', 'IdentifyWrite')
        ForbidInVerdict  = @('PASS', 'no rate answered')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 115200
            UartIdentifyAttempts = 1
            UartFailurePhase     = 'IdentifyWrite'
            UartLastStatus       = 3221225473
            UartAborted          = 0
            UartLastStep         = 72
            UartElapsedMs        = 120
            UartSocId            = 0x00120200
            UartRomVersion       = 0x0201
            UartProductId        = 0
            UartPatchVersion     = 0
            UartIdentifyRawHex   = '04FF110019'
        }
    }
    @{
        Name             = 'uart-identify.ps1 AbortedRecord'
        ExpectedExit     = 1
        IsSuccess        = $false
        RequireInVerdict = @('FAIL', 'aborted=1')
        ForbidInVerdict  = @('PASS', 'no rate answered')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 115200
            UartIdentifyAttempts = 1
            UartFailurePhase     = 'IdentifyWait'
            UartLastStatus       = 3221225760
            UartAborted          = 1
            UartLastStep         = 75
            UartElapsedMs        = 250
            UartSocId            = 0
            UartRomVersion       = 0
            UartProductId        = 0
            UartPatchVersion     = 0
            UartIdentifyRawHex   = $null
        }
    }
    @{
        Name             = 'uart-identify.ps1 GenuineNoResponse'
        ExpectedExit     = 1
        IsSuccess        = $false
        RequireInVerdict = @('FAIL', 'no rate answered')
        ForbidInVerdict  = @('PASS', 'transport error', 'MissingSerialResource')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 0
            UartIdentifyAttempts = 3
            UartFailurePhase     = 'NoResponse'
            UartLastStatus       = 3221226021
            UartAborted          = 0
            UartLastStep         = 78
            UartElapsedMs        = 4500
            UartSocId            = 0
            UartRomVersion       = 0
            UartProductId        = 0
            UartPatchVersion     = 0
            UartIdentifyRawHex   = $null
        }
    }
    @{
        Name             = 'uart-identify.ps1 GenuineAnswered'
        ExpectedExit     = 0
        IsSuccess        = $true
        RequireInVerdict = @('PASS', '3000000 baud')
        ForbidInVerdict  = @('FAIL')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 3000000
            UartIdentifyAttempts = 2
            UartFailurePhase     = 'Answered'
            UartLastStatus       = 0
            UartAborted          = 0
            UartLastStep         = 70
            UartElapsedMs        = 1600
            UartSocId            = 0x00120200
            UartRomVersion       = 0x0201
            UartProductId        = 0x00000008
            UartPatchVersion     = 0x0111
            UartIdentifyRawHex   = '04FF110019'
        }
    }
    @{
        Name             = 'uart-identify.ps1 MissingSerialNotAttempted'
        ExpectedExit     = 1
        IsSuccess        = $false
        RequireInVerdict = @('FAIL', 'MissingSerialResource')
        ForbidInVerdict  = @('PASS', 'no rate answered')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 0
            UartIdentifyAttempts = 0
            UartFailurePhase     = 'MissingSerialResource'
            UartLastStatus       = 3221226021
            UartAborted          = 0
            UartLastStep         = 61
            UartElapsedMs        = 0
            UartSocId            = 0
            UartRomVersion       = 0
            UartProductId        = 0
            UartPatchVersion     = 0
            UartIdentifyRawHex   = $null
        }
    }
    @{
        Name             = 'uart-identify.ps1 IncompleteAnsweredMissingStatus'
        ExpectedExit     = 1
        IsSuccess        = $false
        RequireInVerdict = @('FAIL', 'Answered')
        ForbidInVerdict  = @('PASS')
        ProbeRecord      = [pscustomobject]@{
            UartIdentifyRan      = 1
            UartCompletion       = 1
            UartIdentifyBaud     = 3000000
            UartIdentifyAttempts = 2
            UartFailurePhase     = 'Answered'
            UartLastStatus       = $null
            UartAborted          = 0
            UartLastStep         = 70
            UartElapsedMs        = 1600
            UartSocId            = 0x00120200
            UartRomVersion       = 0x0201
            UartProductId        = 0x00000008
            UartPatchVersion     = 0x0111
            UartIdentifyRawHex   = '04FF110019'
        }
    }
)


$tokens = $null
$parseErrors = $null
$idAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uart-identify.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors) { throw ($parseErrors | Out-String) }
$idMain = $idAst.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] } | Select-Object -Last 1
$idFlow = $idMain.Body.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq '$DryRun'
} | Select-Object -First 1
$idStatements = @($idFlow.ElseClause.Statements)
$mIndex = 0
while ($mIndex -lt $idStatements.Count -and $idStatements[$mIndex].Extent.Text -ne '$script:mutated = $true') { $mIndex++ }
$idExecute = [scriptblock]::Create(($idStatements[$mIndex..($idStatements.Count - 1)] | ForEach-Object { $_.Extent.Text }) -join "`n")
$idCatch = [scriptblock]::Create(($idMain.CatchClauses[0].Body.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")
$idRestore = [scriptblock]::Create(($idMain.Finally.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")

foreach ($cc in $identifyConsumerCases) {
    Run-NamedScenario $cc.Name {
        foreach ($def in $idAst.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Get-Target','Invoke-Pnp')
        }, $true)) { . ([scriptblock]::Create($def.Extent.Text)) }

        $KeepBound = $false
        $DryRun = $false
        $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'
        $RadioChildLike = 'QCA_SHB\UART_H4'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'
        $driverInfName = 'deckbtusb.inf'
        $driverSysName = 'deckbtusb.sys'
        $driverCatName = 'deckbtusb.cat'
        $packageDir = 'mock'
        $driverInf = 'mock\deckbtusb.inf'
        $VendorService = 'QcBluetooth'
        $VendorStoreDir = 'mock\store'
        $VendorBackupDir = 'mock\backup'
        $vendorInfStore = 'mock\store\qcbtuart.inf'
        $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $DeckBaselineJson = 'mock\baseline.json'
        $paramsKey = 'mock-registry'
        $probeWaitSeconds = 15
        $PnpTimeoutSeconds = 10
        $LogPath = 'mock.log'

        $script:scenario = $cc.Name
        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:forbiddenSeen = $false
        $script:packagePresent = $false
        $script:restored = $false
        $script:mutated = $false
        $script:rootAttempted = $false
        $script:packageAttempted = $false
        $script:stagedOemInf = $null
        $script:wedged = $false
        $script:transcriptStarted = $false
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'

        function Get-PnpDevice {
            [CmdletBinding()]
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = $targetId; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = $RadioChildLike; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
        }

        function Get-PnpDeviceProperty {
            [CmdletBinding()]
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    $svc = if ($InstanceId -like "$RadioChildLike*") {
                        'BthMini'
                    } elseif ($script:restored) {
                        $VendorService
                    } else {
                        $driverService
                    }
                    return [pscustomobject]@{ Data = $svc }
                }
                'DEVPKEY_Device_ProblemStatus' {
                    return [pscustomobject]@{ Data = [uint32]0 }
                }
                default { throw "Unexpected property query: $KeyName" }
            }
        }

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = $driverInfName; IsOurs = $true })
            }
            return @()
        }

        function Get-ProbeValues {
            $script:trace.Add('poll-results')
            return $cc.ProbeRecord
        }

        function Test-DeckElevated { return $true }
        function Test-Path {
            [CmdletBinding()]
            param([Parameter(Position = 0)][string] $LiteralPath, [string] $Path)
            $p = if ($LiteralPath) { $LiteralPath } else { $Path }
            if ($p -eq $vendorInfStore) { return $false }
            return $true
        }
        function Get-RootClaimingPackages { return @() }
        function Invoke-DeckNative { param($Exe, $Args) return [pscustomobject]@{ Code = 0; Output = '' } }

        try {
            . $idExecute
        } catch {
            . $idCatch
        } finally {
            . $idRestore
        }

        if ($script:primaryExit -ne $cc.ExpectedExit) {
            throw "Expected exit $($cc.ExpectedExit), got $($script:primaryExit): $($script:verdict)"
        }
        if ($cc.IsSuccess) {
            if ($script:verdict -notmatch '^PASS') { throw "Expected PASS verdict, got: $($script:verdict)" }
        } else {
            if ($script:verdict -match '^PASS') { throw "Expected FAIL verdict, got: $($script:verdict)" }
        }
        foreach ($req in $cc.RequireInVerdict) {
            if ($script:verdict -notmatch [regex]::Escape($req)) {
                throw "Verdict lost expected cause '$req': $($script:verdict)"
            }
        }
        foreach ($forbid in $cc.ForbidInVerdict) {
            if ($script:verdict -match [regex]::Escape($forbid)) {
                throw "Verdict contained forbidden text '$forbid': $($script:verdict)"
            }
        }

        if ($script:packagePresent) { throw 'Package state was not cleaned in restore.' }
        if (-not (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0)) {
            throw 'Physical restore was not verified in restore path.'
        }
        if ($script:showRecoveryCount -ne 0) { throw 'Physical test invoked Show-Recovery.' }
    }
}
Write-Host 'Operator physical restore scenarios (AST-extracted flow against in-memory mock):'

foreach ($opName in @('uart-identify.ps1', 'uart-probe.ps1')) {
    $opAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir $opName), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors) { throw ($parseErrors | Out-String) }
    $opMain = $opAst.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] } | Select-Object -Last 1
    $opFlow = $opMain.Body.Statements | Where-Object {
        $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq '$DryRun'
    } | Select-Object -First 1
    $opStatements = @($opFlow.ElseClause.Statements)
    $mIndex = 0
    while ($mIndex -lt $opStatements.Count -and $opStatements[$mIndex].Extent.Text -ne '$script:mutated = $true') { $mIndex++ }
    $opExecute = [scriptblock]::Create(($opStatements[$mIndex..($opStatements.Count - 1)] | ForEach-Object { $_.Extent.Text }) -join "`n")
    $opFullFlow = [scriptblock]::Create($opFlow.ElseClause.Extent.Text.Trim().TrimStart('{').TrimEnd('}'))
    $opCatch = [scriptblock]::Create(($opMain.CatchClauses[0].Body.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")
    $opRestore = [scriptblock]::Create(($opMain.Finally.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")

    foreach ($def in $opAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Get-Target','Invoke-Pnp')
    }, $true)) { . ([scriptblock]::Create($def.Extent.Text)) }

    Run-NamedScenario "$opName PhysicalCleanRestore" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'
        $script:packagePresent = $true

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'QcBluetooth' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 0) { throw "Expected exit 0 for clean restore, got $($script:primaryExit): $($script:verdict)" }
        if (-not (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0)) {
            throw 'Physical clean restore did not verify.'
        }
        if ($script:showRecoveryCount -ne 0) { throw 'Show-Recovery was called during clean restore.' }
        if (-not $script:deletedPackages.Contains('oem999.inf')) { throw 'Staged package oem999.inf was not deleted.' }
    }

    Run-NamedScenario "$opName PhysicalWrongChildOwner" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'PASS: prior step'
        $script:packagePresent = $true

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'QcBluetooth' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'WrongService' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 1) { throw "Wrong child service must fail restore with exit 1; got $($script:primaryExit)" }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0) {
            throw 'Wrong child service was incorrectly verified as healthy.'
        }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called when child service is wrong.' }
        if ($script:verdict -notmatch 'FAIL: restore not verified') { throw "Verdict did not reflect restore failure: $($script:verdict)" }
    }

    Run-NamedScenario "$opName PhysicalAmbiguousTarget" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'PASS: prior step'
        $script:packagePresent = $true

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return @(
                    [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' },
                    [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\1'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
                )
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'QcBluetooth' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 1) { throw "Ambiguous target must fail restore with exit 1; got $($script:primaryExit)" }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0) {
            throw 'Ambiguous target was incorrectly verified as healthy.'
        }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called when target is ambiguous.' }
    }

    Run-NamedScenario "$opName PhysicalUnreadableTarget" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'PASS: prior step'
        $script:packagePresent = $true

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            throw 'Simulated PnP query failure'
        }

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 1) { throw "Unreadable target must fail restore with exit 1; got $($script:primaryExit)" }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0) {
            throw 'Unreadable target was incorrectly verified as healthy.'
        }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called when target is unreadable.' }
    }

    Run-NamedScenario "$opName PhysicalForeignPackageSurvives" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'

        $script:stagedPool = [System.Collections.Generic.List[object]]::new()
        $script:stagedPool.Add([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        $script:stagedPool.Add([pscustomobject]@{ Published = 'oem111.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })

        function Get-ProbePackages {
            return @($script:stagedPool | Where-Object { -not $script:deletedPackages.Contains($_.Published) })
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'QcBluetooth' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }

        try { . $opRestore } catch { . $opCatch }

        if (-not $script:deletedPackages.Contains('oem999.inf')) {
            throw 'Run-owned package oem999.inf was not deleted.'
        }
        if ($script:deletedPackages.Contains('oem111.inf')) {
            throw 'Foreign/pre-existing package oem111.inf was incorrectly deleted in physical restore!'
        }
        $surviving = @(Get-ProbePackages)
        if ($surviving.Count -ne 1 -or $surviving[0].Published -ne 'oem111.inf') {
            throw 'Foreign/pre-existing package oem111.inf did not survive.'
        }
    }

    Run-NamedScenario "$opName PhysicalTimeoutWedgedNoFurtherAccess" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $true
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 1
        $script:verdict = 'FAIL: timed out: identify step'

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 1) { throw "Expected exit 1 when wedged, got $($script:primaryExit)" }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called for wedged restore.' }
        $disallowed = @($script:trace | Where-Object { $_ -match 'poll-target|enable-device|restart-device|scan-devices|delete-package|verify-radio-health' })
        if ($disallowed.Count -gt 0) {
            throw "Wedged state performed disallowed PnP/target operations: $($disallowed -join ', ')"
        }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED' }).Count -gt 0) {
            throw 'Wedged restore emitted RESTORE VERIFIED.'
        }
    }

    Run-NamedScenario "$opName PhysicalRestoreFailureOverridesSuccess" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $true
        $script:packageAttempted = $true
        $script:stagedOemInf = 'oem999.inf'
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'PASS: prior probe succeeded'
        $script:packagePresent = $true

        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'Error'; Problem = 'CM_PROB_FAILED_POST' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'QcBluetooth' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }

        try { . $opRestore } catch { . $opCatch }

        if ($script:primaryExit -ne 1) { throw "Recovery failure must override exit to 1, got $($script:primaryExit)" }
        if ($script:verdict -match '^PASS') { throw "Recovery failure must override PASS verdict, got $($script:verdict)" }
        if ($script:verdict -notmatch 'FAIL: restore not verified') { throw "Verdict did not reflect restore failure: $($script:verdict)" }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called when restore failed.' }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED' }).Count -gt 0) {
            throw 'RESTORE VERIFIED was emitted despite radio error.'
        }
    }

    Run-NamedScenario "$opName PhysicalPreexistingPackageRefused" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $driverSysName = 'deckbtusb.sys'; $driverCatName = 'deckbtusb.cat'
        $packageDir = 'mock'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $DeckBaselineJson = 'mock\baseline.json'
        $paramsKey = 'mock-params'
        $PnpTimeoutSeconds = 10
        $LogPath = 'mock.log'

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $false
        $script:packageAttempted = $false
        $script:stagedOemInf = $null
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'

        function Test-DeckElevated { return $true }
        function Test-Path { return $true }
        function Get-ProbePackages {
            return @([pscustomobject]@{ Published = 'oem111.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'Error'; Problem = 'CM_PROB_DISABLED' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            return [pscustomobject]@{ Data = 'QcBluetooth' }
        }
        function New-Item { throw 'Refusal must occur before mutation.' }
        function Start-Transcript { throw 'Refusal must occur before starting transcript.' }

        try { . $opFullFlow } catch { . $opCatch } finally { . $opRestore }

        if ($script:primaryExit -ne 2) { throw "Pre-existing matching package must refuse with exit 2, got $($script:primaryExit)" }
        if ($script:mutated) { throw "Pre-existing matching package mutated state despite refusal." }
        if ($script:verdict -notmatch 'REFUSAL: a matching project package is already staged') {
            throw "Verdict did not name package refusal: $($script:verdict)"
        }
    }
    foreach ($initiallyDisabled in @($true, $false)) {
        Run-NamedScenario "$opName ProgressToCompletion (disabled=$initiallyDisabled)" {
            $KeepBound = $false; $DryRun = $false; $Force = $true
            $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
            $targetId = $RadioAcpiId
            $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
            $driverSysName = 'deckbtusb.sys'; $driverCatName = 'deckbtusb.cat'
            $packageDir = 'mock'
            $driverInf = 'mock\deckbtusb.inf'
            $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
            $VendorService = 'QcBluetooth'
            $DeckBaselineJson = 'mock\baseline.json'
            $paramsKey = 'mock-params'
            $probeWaitSeconds = 5
            $PnpTimeoutSeconds = 10
            $LogPath = 'mock.log'

            $script:trace = [System.Collections.Generic.List[string]]::new()
            $script:capturedHost.Clear()
            $script:showRecoveryCount = 0
            $script:wedged = $false
            $script:mutated = $false
            $script:packageAttempted = $false
            $script:stagedOemInf = $null
            $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
            $script:primaryExit = 0
            $script:verdict = 'NOT RUN'
            $script:packagePresent = $false
            $script:restored = $false
            $script:pollCount = 0
            $lifecycle = @{
                Disabled = $initiallyDisabled
                Armed = $false
                Started = $false
                Running = $false
                Cancelled = $false
                Completed = $false
            }
            $baseWorker = ${function:Invoke-Bounded}

            function Start-MockProbe {
                if ($lifecycle.Armed -and -not $lifecycle.Started) {
                    $lifecycle.Armed = $false
                    $lifecycle.Started = $true
                    $lifecycle.Running = $true
                }
            }
            function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments, [int] $TimeoutSeconds) {
                if ($Label -like 'Clear stale results and arm Uart*') { $lifecycle.Armed = $true }
                if ($Label -like 'pnputil *') {
                    $native = @($Arguments[0])
                    switch ($native[0]) {
                        '/add-driver' {
                            if ($native -contains '/install' -and $native[1] -eq $driverInf -and -not $lifecycle.Disabled) {
                                Start-MockProbe
                            }
                        }
                        '/enable-device' {
                            if (-not $lifecycle.Disabled) {
                                # Windows 11 Home pnputil refuses to enable an enabled device
                                return [pscustomobject]@{ Code = 50; Output = 'Device is already enabled. This command is not supported on this OS product.' }
                            }
                            $lifecycle.Disabled = $false
                            Start-MockProbe
                        }
                        '/restart-device' {
                            # A restart enters the driver's D0Exit/ReleaseHardware cancellation path.
                            if ($lifecycle.Running) { $lifecycle.Cancelled = $true }
                            Start-MockProbe
                        }
                        '/delete-driver' {
                            if ($lifecycle.Running) { $lifecycle.Cancelled = $true }
                        }
                    }
                }
                & $baseWorker $Label $Action $Arguments
            }
            function Test-DeckElevated { return $true }
            function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
            function Get-ProbePackages {
                if ($script:packagePresent) {
                    return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
                }
                return @()
            }
            function Get-PnpDevice {
                param([string] $InstanceId, [switch] $PresentOnly)
                if ($InstanceId -like "$targetId*") {
                    return [pscustomobject]@{
                        InstanceId = 'ACPI\QCOM2066\0'
                        Status = if ($lifecycle.Disabled) { 'Error' } else { 'OK' }
                        Problem = if ($lifecycle.Disabled) { 'CM_PROB_DISABLED' } else { 'CM_PROB_NONE' }
                    }
                }
                if ($InstanceId -like "$RadioChildLike*") {
                    return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
                }
                return $null
            }
            function Get-PnpDeviceProperty {
                param([string] $InstanceId, [string] $KeyName)
                switch ($KeyName) {
                    'DEVPKEY_Device_Service' {
                        if ($InstanceId -like '*QCOM2066*') {
                            return [pscustomobject]@{ Data = if ($script:restored) { 'QcBluetooth' } else { 'DeckBtUsb' } }
                        }
                        if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                    }
                    'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
                }
                return $null
            }
            function Get-ProbeValues {
                param([int] $TimeoutSeconds)
                $script:pollCount++
                if ($lifecycle.Started -and -not $lifecycle.Cancelled -and $script:pollCount -ge 2) {
                    $lifecycle.Running = $false
                    $lifecycle.Completed = $true
                }
                $completion = if ($lifecycle.Cancelled) { 2 } elseif ($lifecycle.Completed) { 1 } else { 0 }
                return [pscustomobject]@{
                    UartIdentifyRan      = [int]$lifecycle.Started
                    UartProbeRan         = [int]$lifecycle.Started
                    UartCompletion       = $completion
                    UartIdentifyBaud     = 3000000
                    UartIdentifyAttempts = 1
                    UartFailurePhase     = 'Answered'
                    UartLastStatus       = 0
                    UartAborted          = [int]$lifecycle.Cancelled
                    UartSerialOpened     = 1
                    UartPatchBytesSent   = 100
                    UartNvmBytesSent     = 100
                    UartHciResetSent     = 1
                    UartHciResetStatus   = 0
                    UartHciResetEventLen = 4
                    UartSocId            = 0x00120200
                    UartRomVersion       = 0x0201
                    UartProductId        = 0x00000008
                    UartPatchVersion     = 0x0111
                }
            }
            function Start-Transcript { }

            try { . $opExecute } catch { . $opCatch } finally { . $opRestore }

            if ($lifecycle.Cancelled) { throw 'The operator cancelled its own probe before UART retirement.' }
            if (-not $lifecycle.Completed) { throw 'The probe never reached confirmed retirement.' }
            if ($script:primaryExit -ne 0) { throw "Expected successful identification/probe, got $($script:primaryExit): $($script:verdict)" }
            if (-not $script:restored) { throw 'The vendor driver was not restored after the completed probe.' }
        }
    }

    # pnputil /install can succeed while Windows refuses to load the driver. The driver then never
    # writes fresh results, so a stale completed record from an earlier run must not become a PASS.
    Run-NamedScenario "$opName DriverLoadFailure" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $driverSysName = 'deckbtusb.sys'; $driverCatName = 'deckbtusb.cat'
        $packageDir = 'mock'
        $driverInf = 'mock\deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $DeckBaselineJson = 'mock\baseline.json'
        $paramsKey = 'mock-params'
        $probeWaitSeconds = 5
        $PnpTimeoutSeconds = 10
        $LogPath = 'mock.log'

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $false
        $script:packageAttempted = $false
        $script:stagedOemInf = $null
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'
        $script:packagePresent = $false
        $script:restored = $false
        $script:installed = $false
        $baseWorker = ${function:Invoke-Bounded}

        function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments, [int] $TimeoutSeconds) {
            if ($Label -like 'pnputil *') {
                $native = @($Arguments[0])
                if ($native[0] -eq '/add-driver' -and $native -contains '/install' -and $native[1] -eq $driverInf) {
                    $script:installed = $true
                }
            }
            & $baseWorker $Label $Action $Arguments
        }
        function Test-DeckElevated { return $true }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Test-LoadFailed { return ($script:installed -and -not $script:restored) }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{
                    InstanceId = 'ACPI\QCOM2066\0'
                    Status = if (Test-LoadFailed) { 'Error' } else { 'OK' }
                    Problem = if (Test-LoadFailed) { 'CM_PROB_DRIVER_FAILED_LOAD' } else { 'CM_PROB_NONE' }
                }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') {
                        return [pscustomobject]@{ Data = if ($script:restored) { 'QcBluetooth' } else { 'DeckBtUsb' } }
                    }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' {
                    return [pscustomobject]@{ Data = if (Test-LoadFailed) { [Convert]::ToUInt32('C0E90002', 16) } else { [uint32]0 } }
                }
            }
            return $null
        }
        function Get-ProbeValues {
            param([int] $TimeoutSeconds)
            $script:trace.Add('poll-results')
            # Stale record of an earlier successful run: exactly what a failed load leaves behind.
            return [pscustomobject]@{
                UartIdentifyRan      = 1
                UartProbeRan         = 1
                UartCompletion       = 1
                UartIdentifyBaud     = 115200
                UartIdentifyAttempts = 1
                UartFailurePhase     = 'Answered'
                UartLastStatus       = 0
                UartAborted          = 0
                UartSerialOpened     = 1
                UartPatchBytesSent   = 100
                UartNvmBytesSent     = 100
                UartHciResetSent     = 1
                UartHciResetStatus   = 0
                UartHciResetEventLen = 4
                UartSocId            = 0x00120200
                UartRomVersion       = 0x0201
                UartProductId        = 0x00000008
                UartPatchVersion     = 0x0111
            }
        }
        function Start-Transcript { }

        try { . $opExecute } catch { . $opCatch } finally { . $opRestore }

        if (-not $script:installed) { throw 'The scenario never reached package installation.' }
        if ($script:primaryExit -ne 1) { throw "A driver that failed to load was reported as exit $($script:primaryExit): $($script:verdict)" }
        if (-not $script:verdict.Contains('CM_PROB_DRIVER_FAILED_LOAD')) { throw "Load failure lost its problem code: $($script:verdict)" }
        if ($script:trace.Contains('poll-results')) { throw 'Stale driver results were read after the device reported a load failure.' }
        if (-not $script:restored) { throw 'The vendor driver was not restored after the load failure.' }
    }

    Run-NamedScenario "$opName Completion2AbortedNoMutations" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $driverSysName = 'deckbtusb.sys'; $driverCatName = 'deckbtusb.cat'
        $packageDir = 'mock'
        $driverInf = 'mock\deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $DeckBaselineJson = 'mock\baseline.json'
        $paramsKey = 'mock-params'
        $probeWaitSeconds = 5
        $PnpTimeoutSeconds = 10
        $LogPath = 'mock.log'

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $false
        $script:packageAttempted = $false
        $script:stagedOemInf = $null
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'
        $script:packagePresent = $false
        $terminalBoundary = @{ Observed = $false; Violations = [System.Collections.Generic.List[string]]::new() }
        $baseWorker = ${function:Invoke-Bounded}
        function Assert-MockStateAccess([string] $Operation) {
            if ($terminalBoundary.Observed) {
                $terminalBoundary.Violations.Add($Operation)
                throw "State access after unconfirmed retirement: $Operation"
            }
        }
        function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments, [int] $TimeoutSeconds) {
            Assert-MockStateAccess $Label
            & $baseWorker $Label $Action $Arguments
        }
        $recoveryDef = $opAst.Find({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Show-Recovery'
        }, $true)
        . ([scriptblock]::Create($recoveryDef.Extent.Text))

        function Test-DeckElevated { return $true }
        function Test-Path { param($LiteralPath) Assert-MockStateAccess 'Test-Path'; if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-ProbePackages {
            Assert-MockStateAccess 'Get-ProbePackages'
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            Assert-MockStateAccess 'Get-PnpDevice'
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            Assert-MockStateAccess 'Get-PnpDeviceProperty'
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'DeckBtUsb' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }
        function Get-ProbeValues {
            param([int] $TimeoutSeconds)
            Assert-MockStateAccess 'Get-ProbeValues'
            $terminalBoundary.Observed = $true
            return [pscustomobject]@{
                UartIdentifyRan  = 1
                UartProbeRan     = 1
                UartCompletion   = 2
                UartFailurePhase = 'WatchdogCancellation'
                UartAborted      = 1
                UartLastStatus   = 3221225760
                UartLastStep     = 91
                UartElapsedMs    = 1200
                LastAddDeviceStep   = 92
                LastAddDeviceStatus = 0
            }
        }
        function Start-Transcript { }

        try { . $opExecute } catch { . $opCatch } finally { . $opRestore }

        if ($script:primaryExit -ne 1) { throw "Completion=2 must fail with exit 1, got $($script:primaryExit)" }
        if (-not $script:wedged) { throw 'Completion=2 did not set script:wedged=$true' }
        if ($terminalBoundary.Violations.Count) { throw "Further state access attempted: $($terminalBoundary.Violations -join ', ')" }
        $fatalOutput = $script:capturedHost -join "`n"
        if ($fatalOutput -notmatch 'UartLastStatus\s*:\s*3221225760') { throw 'The captured cancellation status was lost from the report.' }
        if ($fatalOutput -notmatch 'LastAddDeviceStep\s*:\s*92' -or $fatalOutput -notmatch 'LastAddDeviceStatus\s*:\s*0') {
            throw 'The already-captured low-level breadcrumb was lost from the report.'
        }
        if ($fatalOutput -match 'pnputil\s+/|reg delete|sc delete|tools\\session\.ps1 -Uninstall') {
            throw 'Unconfirmed retirement must not recommend immediate uninstall/rollback commands.'
        }
        if (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED' }).Count -gt 0) {
            throw 'Completion=2 incorrectly emitted RESTORE VERIFIED.'
        }
        if ($script:deletedPackages.Count -gt 0) {
            throw "Completion=2 must not attempt automatic teardown/package deletion; deleted: $($script:deletedPackages -join ', ')"
        }
    }

    Run-NamedScenario "$opName MissingMarkerBoundedRefusal" {
        $KeepBound = $false; $DryRun = $false; $Force = $true
        $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
        $targetId = $RadioAcpiId
        $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
        $driverSysName = 'deckbtusb.sys'; $driverCatName = 'deckbtusb.cat'
        $packageDir = 'mock'
        $driverInf = 'mock\deckbtusb.inf'
        $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
        $VendorService = 'QcBluetooth'
        $DeckBaselineJson = 'mock\baseline.json'
        $paramsKey = 'mock-params'
        $probeWaitSeconds = 1
        $PnpTimeoutSeconds = 2
        $LogPath = 'mock.log'

        $script:trace = [System.Collections.Generic.List[string]]::new()
        $script:capturedHost.Clear()
        $script:showRecoveryCount = 0
        $script:wedged = $false
        $script:mutated = $false
        $script:packageAttempted = $false
        $script:stagedOemInf = $null
        $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
        $script:primaryExit = 0
        $script:verdict = 'NOT RUN'
        $script:packagePresent = $false

        function Test-DeckElevated { return $true }
        function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
        function Get-ProbePackages {
            if ($script:packagePresent) {
                return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
            }
            return @()
        }
        function Get-PnpDevice {
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($InstanceId -like "$targetId*") {
                return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            if ($InstanceId -like "$RadioChildLike*") {
                return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
            }
            return $null
        }
        function Get-PnpDeviceProperty {
            param([string] $InstanceId, [string] $KeyName)
            switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'DeckBtUsb' } }
                    if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
                }
                'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
            }
            return $null
        }
        function Get-ProbeValues {
            param([int] $TimeoutSeconds)
            return [pscustomobject]@{
                UartIdentifyRan  = 1
                UartProbeRan     = 1
                UartIdentifyBaud = 3000000
                UartFailurePhase = 'Answered'
            }
        }
        function Start-Transcript { }

        try { . $opExecute } catch { . $opCatch } finally { . $opRestore }

        if ($script:primaryExit -ne 1) { throw "Missing marker must fail with exit 1, got $($script:primaryExit)" }
        if (-not $script:wedged) { throw 'Missing marker timeout did not set script:wedged=$true' }
        if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called for missing marker timeout' }
        if ($script:verdict -notmatch 'ownership retirement unconfirmed') {
            throw "Verdict did not name unconfirmed retirement: $($script:verdict)"
        }
        if ($script:deletedPackages.Count -gt 0) {
            throw "Missing marker timeout must not perform automatic teardown; deleted: $($script:deletedPackages -join ', ')"
        }
    }
}
Write-Host 'Bridge operator scenarios (AST-extracted flow against in-memory mock):'

$bridgeAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uart-probe.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors) { throw ($parseErrors | Out-String) }
$bridgeMain = $bridgeAst.EndBlock.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] } | Select-Object -Last 1
$bridgeFlow = $bridgeMain.Body.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq '$DryRun'
} | Select-Object -First 1
$bridgeStatements = @($bridgeFlow.ElseClause.Statements)
$bIndex = 0
while ($bIndex -lt $bridgeStatements.Count -and $bridgeStatements[$bIndex].Extent.Text -ne '$script:mutated = $true') { $bIndex++ }
$bridgeExecute = [scriptblock]::Create(($bridgeStatements[$bIndex..($bridgeStatements.Count - 1)] | ForEach-Object { $_.Extent.Text }) -join "`n")
$bridgeCatch = [scriptblock]::Create(($bridgeMain.CatchClauses[0].Body.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")
$bridgeRestore = [scriptblock]::Create(($bridgeMain.Finally.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")

foreach ($def in $bridgeAst.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -in @('Get-Target','Invoke-Pnp','Show-Ep0Trace','Show-EventTrace','Show-AdvSeen','Show-ScoPath')
}, $true)) { . ([scriptblock]::Create($def.Extent.Text)) }


Run-NamedScenario 'uart-probe.ps1 BridgeCleanRunAndRestore' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $enumWaitSeconds = 1
    $LogPath = 'mock-bridge.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $false
    $script:packageAttempted = $false
    $script:stagedOemInf = $null
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $false
    $script:bthWasRunning = $true
    $script:uninstallRequiresReboot = $false
    $script:mockEnumerators = $null
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()

    $script:bridgePollStep = 0
    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        $script:bridgePollStep++
        if ($script:bridgePollStep -le 2) {
            return [pscustomobject]@{
                UartFailurePhase         = 'Steady'
                UartSteadyReached        = 1
                UartCompletion           = 0
                UartBridgeCommands       = 10
                UartBridgeEventsQueued   = 10
                UartBridgeCommandsFailed = 0
                ControlCount             = 2
                ControlLog               = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C,
                                                         0,0,0,0,0,0,0,0, 1, 0, 0x05, 0x0C)
            }
        } else {
            return [pscustomobject]@{
                UartCompletion     = 1
                UartFailurePhase    = 'Stopped'
                UartHandbackBaud   = 115200
                UartHandbackStatus = 0
            }
        }
    }

    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'USB\VID_0CF3&PID_6390*') {
            return [pscustomobject]@{ InstanceId = 'USB\VID_0CF3&PID_6390\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHBRB*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHBRB\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHLE*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHLE\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like "$RadioChildLike*") {
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' {
                if ($InstanceId -like '*QCOM2066*') {
                    $svc = if ($script:bridgePollStep -ge 3) { 'QcBluetooth' } else { 'DeckBtUsb' }
                    return [pscustomobject]@{ Data = $svc }
                }
                if ($InstanceId -like '*USB*') { return [pscustomobject]@{ Data = 'BTHUSB' } }
                if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
            }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeExecute } catch { . $bridgeCatch } finally { . $bridgeRestore }

    if ($script:primaryExit -ne 0) { throw "Expected exit 0 for bridge clean run, got $($script:primaryExit): $($script:verdict)" }
    if ($script:verdict -ne 'PASS: BTHPORT initialized on the real controller') {
        throw "Unexpected verdict: $($script:verdict)"
    }
    if (-not $script:deletedPackages.Contains('oem999.inf')) { throw 'Staged package oem999.inf was not deleted during teardown.' }
    if ($script:mockRegistry.ContainsKey('Enabled')) { throw 'Bridge teardown left Enabled in registry.' }
    if (-not $script:trace.Contains('delete-package')) { throw 'pnputil /delete-driver was not called during bridge teardown.' }
    if (-not $script:trace.Contains('stop-bthserv')) { throw 'bthserv was not stopped during bridge teardown.' }
    if (-not $script:trace.Contains('restart-bthserv')) { throw 'bthserv was not restarted after bridge restore.' }
    if (-not (@($script:capturedHost | Where-Object { $_ -match 'RESTORE VERIFIED: QcBluetooth owns the radio and the child is healthy\.' }).Count -gt 0)) {
        throw 'Physical restore verification line was not observed.'
    }
}

Run-NamedScenario 'uart-probe.ps1 BridgeUnconfirmedReleaseRefusesRestore' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $LogPath = 'mock-bridge.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $true
    $script:packageAttempted = $true
    $script:stagedOemInf = 'oem999.inf'
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $true
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $false
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()
    $script:mockRegistry['Enabled'] = 1

    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        return [pscustomobject]@{
            UartCompletion     = 0
            UartFailurePhase    = 'Steady'
            UartHandbackBaud   = 0
            UartHandbackStatus = 0
        }
    }
    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' { return [pscustomobject]@{ Data = 'DeckBtUsb' } }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeRestore } catch { . $bridgeCatch }

    if ($script:primaryExit -ne 1) { throw "Unconfirmed release must exit 1, got $($script:primaryExit)" }
    if (-not $script:wedged) { throw 'Unconfirmed release must set script:wedged=$true' }
    if ($script:showRecoveryCount -eq 0) { throw 'Show-Recovery was not called when release unconfirmed.' }
    if (-not $script:mockRegistry.ContainsKey('Enabled')) { throw 'Unconfirmed release incorrectly removed Enabled parameter.' }
    if ($script:trace.Contains('scan-devices')) { throw 'Unconfirmed release incorrectly proceeded to vendor scan-devices.' }
}

Run-NamedScenario 'uart-probe.ps1 BridgeLeavesEnabledDisarmed' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $LogPath = 'mock-bridge.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $true
    $script:packageAttempted = $true
    $script:stagedOemInf = 'oem999.inf'
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $true
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $false
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()
    $script:mockRegistry['Enabled'] = 1

    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        return [pscustomobject]@{
            UartCompletion     = 1
            UartFailurePhase    = 'Stopped'
            UartHandbackBaud   = 115200
            UartHandbackStatus = 0
        }
    }
    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like "$RadioChildLike*") {
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' { return [pscustomobject]@{ Data = 'QcBluetooth' } }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeRestore } catch { . $bridgeCatch }

    if ($script:mockRegistry.ContainsKey('Enabled')) {
        throw 'Bridge teardown left Enabled armed in registry.'
    }
}

Run-NamedScenario 'uart-probe.ps1 BridgeUninstallRebootRequired' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $LogPath = 'mock-bridge.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $true
    $script:packageAttempted = $true
    $script:stagedOemInf = 'oem999.inf'
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $true
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $true
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()
    $script:mockRegistry['Enabled'] = 1

    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' { return [pscustomobject]@{ Data = 'DeckBtUsb' } }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeRestore } catch { . $bridgeCatch }

    if ($script:primaryExit -ne 1) { throw "Reboot required must exit 1, got $($script:primaryExit)" }
    if ($script:verdict -notmatch 'REBOOT REQUIRED') { throw "Expected REBOOT REQUIRED in verdict, got $($script:verdict)" }
    if ($script:trace.Contains('scan-devices')) { throw 'Reboot required must skip vendor restore.' }
    $hostText = $script:capturedHost -join "`n"
    if ($hostText -notmatch 'Exact next steps after reboot:') {
        throw 'Reboot-required path did not print exact next steps.'
    }
}

Run-NamedScenario 'uart-probe.ps1 BridgeFalsePassRefusedWhenEnumeratorsAbsent' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $enumWaitSeconds = 1
    $LogPath = 'mock-bridge.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $false
    $script:packageAttempted = $false
    $script:stagedOemInf = $null
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $false
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $false
    $script:mockEnumerators = [pscustomobject]@{ BrbPresent = $false; BrbStatus = $null; LePresent = $false; LeStatus = $null }
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()

    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        return [pscustomobject]@{
            UartFailurePhase         = 'Steady'
            UartSteadyReached        = 1
            UartCompletion           = 0
            UartBridgeCommands       = 10
            UartBridgeEventsQueued   = 10
            UartBridgeCommandsFailed = 0
            ControlCount                = 2
            ControlLog                  = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C,
                                                     0,0,0,0,0,0,0,0, 1, 0, 0x05, 0x0C)
        }
    }

    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'USB\VID_0CF3&PID_6390*') {
            return [pscustomobject]@{ InstanceId = 'USB\VID_0CF3&PID_6390\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' {
                if ($InstanceId -like '*QCOM2066*') { return [pscustomobject]@{ Data = 'DeckBtUsb' } }
                if ($InstanceId -like '*USB*') { return [pscustomobject]@{ Data = 'BTHUSB' } }
            }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeExecute } catch { . $bridgeCatch }

    if ($script:primaryExit -ne 1) { throw "Absent enumerators must exit 1, got $($script:primaryExit)" }
    if ($script:verdict -match 'PASS') { throw "Verdict must not be PASS when enumerators are absent: $($script:verdict)" }
    if ($script:verdict -notmatch 'FAIL: Bridge criteria not met') {
        throw "Verdict did not reflect criteria failure: $($script:verdict)"
    }
}
Run-NamedScenario 'uart-probe.ps1 BridgeHoldLoopTimeoutAndTeardown' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $enumWaitSeconds = 1
    $HoldSeconds = 1
    $LogPath = 'mock-bridge-hold.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $false
    $script:packageAttempted = $false
    $script:stagedOemInf = $null
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $false
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $false
    $script:mockEnumerators = $null
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()

    $script:bridgePollStep = 0
    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        $script:bridgePollStep++
        if ($script:bridgePollStep -le 5) {
            return [pscustomobject]@{
                UartFailurePhase         = 'Steady'
                UartSteadyReached        = 1
                UartCompletion           = 0
                UartBridgeCommands       = 10
                UartBridgeEventsQueued   = 10
                UartBridgeCommandsFailed = 0
                UartAdvReports           = 5
                UartBridgeAclOut         = 3
                UartBridgeAclIn          = 4
                UartBridgeEventsReceived = 15
                EventCount                  = 2
                EventLog                    = [byte[]]@(0x03, 0x06, 0x00, 0x40, 0x00, 0x66, 0x55, 0x44,
                                                         0x08, 0x04, 0x00, 0x40, 0x00, 0x01, 0x00, 0x00)
                ControlCount                = 1
                ControlLog                  = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C)
            }
        } else {
            return [pscustomobject]@{
                UartCompletion     = 1
                UartFailurePhase    = 'Stopped'
                UartHandbackBaud   = 115200
                UartHandbackStatus = 0
            }
        }
    }

    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'USB\VID_0CF3&PID_6390*') {
            return [pscustomobject]@{ InstanceId = 'USB\VID_0CF3&PID_6390\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHBRB*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHBRB\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHLE*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHLE\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like "$RadioChildLike*") {
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' {
                if ($InstanceId -like '*QCOM2066*') {
                    $svc = if ($script:bridgePollStep -ge 6) { 'QcBluetooth' } else { 'DeckBtUsb' }
                    return [pscustomobject]@{ Data = $svc }
                }
                if ($InstanceId -like '*USB*') { return [pscustomobject]@{ Data = 'BTHUSB' } }
                if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
            }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    try { . $bridgeExecute } catch { . $bridgeCatch } finally { . $bridgeRestore }

    if ($script:primaryExit -ne 0) { throw "Expected exit 0 for hold run, got $($script:primaryExit): $($script:verdict)" }
    $hostText = $script:capturedHost -join "`n"
    if ($hostText -notmatch 'PAIRING EVIDENCE:') { throw 'Pairing evidence was not printed.' }
    if ($hostText -notmatch 'HID REPORT EVIDENCE:') { throw 'HID report evidence was not printed.' }
    if ($hostText -notmatch 'connections=1 encryption=1 aclOut=3 aclIn=4') {
        throw "Pairing evidence counts incorrect: $hostText"
    }
    if (-not $script:deletedPackages.Contains('oem999.inf')) { throw 'Package was not deleted during hold teardown.' }
    if ($script:mockRegistry.ContainsKey('Enabled')) { throw 'Enabled parameter remained after hold teardown.' }
}

Run-NamedScenario 'uart-probe.ps1 BridgeVerdictParsing' {
    $Bridge = $true; $KeepBound = $false; $DryRun = $false; $Force = $true
    $RadioAcpiId = 'ACPI\QCOM2066'; $RadioChildLike = 'QCA_SHB\UART_H4*'
    $targetId = $RadioAcpiId
    $driverService = 'DeckBtUsb'; $driverInfName = 'deckbtusb.inf'
    $vendorInfStore = 'mock\store\qcbtuart.inf'; $vendorInfBackup = 'mock\backup\qcbtuart.inf'
    $VendorService = 'QcBluetooth'
    $paramsKey = 'mock-params'
    $PnpTimeoutSeconds = 10
    $releaseWaitSeconds = 1
    $enumWaitSeconds = 1
    $HoldSeconds = 0
    $LogPath = 'mock-bridge-scan.log'

    $script:trace = [System.Collections.Generic.List[string]]::new()
    $script:capturedHost.Clear()
    $script:showRecoveryCount = 0
    $script:wedged = $false
    $script:mutated = $false
    $script:packageAttempted = $false
    $script:stagedOemInf = $null
    $script:deletedPackages = [System.Collections.Generic.List[string]]::new()
    $script:primaryExit = 0
    $script:verdict = 'NOT RUN'
    $script:packagePresent = $false
    $script:bthWasRunning = $false
    $script:uninstallRequiresReboot = $false
    $script:mockEnumerators = $null
    $script:mockRegistry = [System.Collections.Generic.Dictionary[string, object]]::new()

    $script:scanStep = 0
    function Get-ProbeValues {
        param([int] $TimeoutSeconds)
        $script:scanStep++
        if ($script:scanStep -eq 1) {
            return [pscustomobject]@{
                UartFailurePhase         = 'Steady'
                UartSteadyReached        = 1
                UartCompletion           = 0
                UartBridgeCommands       = 10
                UartBridgeEventsQueued   = 10
                UartBridgeCommandsFailed = 0
                UartAdvReports           = 5
                ControlCount             = 1
                ControlLog               = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C)
            }
        } elseif ($script:scanStep -eq 2) {
            return [pscustomobject]@{
                UartFailurePhase         = 'Steady'
                UartSteadyReached        = 1
                UartCompletion           = 0
                UartBridgeCommands       = 10
                UartBridgeEventsQueued   = 10
                UartBridgeCommandsFailed = 0
                UartAdvReports           = 5
                ControlCount             = 1
                ControlLog                  = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C)
            }
        } elseif ($script:scanStep -eq 3) {
            # After scan
            return [pscustomobject]@{
                UartFailurePhase         = 'Steady'
                UartSteadyReached        = 1
                UartCompletion           = 0
                UartBridgeCommands       = 12
                UartBridgeEventsQueued   = 15
                UartBridgeEventsReceived = 15
                UartBridgeCommandsFailed = 0
                UartAdvReports           = 17
                ControlCount             = 1
                ControlLog                  = [byte[]]@(0,0,0,0,0,0,0,0, 1, 0, 0x03, 0x0C)
            }
        } else {
            return [pscustomobject]@{
                UartCompletion     = 1
                UartFailurePhase    = 'Stopped'
                UartHandbackBaud   = 115200
                UartHandbackStatus = 0
            }
        }
    }

    function Get-ProbePackages {
        if ($script:packagePresent) {
            return @([pscustomobject]@{ Published = 'oem999.inf'; OriginalName = 'deckbtusb.inf'; IsOurs = $true })
        }
        return @()
    }
    function Test-Path { param($LiteralPath) if ($LiteralPath -eq $vendorInfStore) { return $false } return $true }
    function Get-PnpDevice {
        param([string] $InstanceId, [switch] $PresentOnly)
        if ($InstanceId -like "$targetId*") {
            return [pscustomobject]@{ InstanceId = 'ACPI\QCOM2066\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'USB\VID_0CF3&PID_6390*') {
            return [pscustomobject]@{ InstanceId = 'USB\VID_0CF3&PID_6390\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHBRB*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHBRB\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like 'BTH\MS_BTHLE*') {
            return [pscustomobject]@{ InstanceId = 'BTH\MS_BTHLE\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        if ($InstanceId -like "$RadioChildLike*") {
            return [pscustomobject]@{ InstanceId = 'QCA_SHB\UART_H4\0'; Status = 'OK'; Problem = 'CM_PROB_NONE' }
        }
        return $null
    }
    function Get-PnpDeviceProperty {
        param([string] $InstanceId, [string] $KeyName)
        switch ($KeyName) {
            'DEVPKEY_Device_Service' {
                if ($InstanceId -like '*QCOM2066*') {
                    $svc = if ($script:scanStep -ge 4) { 'QcBluetooth' } else { 'DeckBtUsb' }
                    return [pscustomobject]@{ Data = $svc }
                }
                if ($InstanceId -like '*USB*') { return [pscustomobject]@{ Data = 'BTHUSB' } }
                if ($InstanceId -like '*UART_H4*') { return [pscustomobject]@{ Data = 'BthMini' } }
            }
            'DEVPKEY_Device_ProblemStatus' { return [pscustomobject]@{ Data = [uint32]0 } }
        }
        return $null
    }
    function Start-Sleep { param($Milliseconds, $Seconds) }

    # Mock Invoke-Bounded to return scan output with unpaired=7
    $savedWorker = ${function:Invoke-Bounded}
    function Invoke-Bounded([string] $Label, [scriptblock] $Action, [object[]] $Arguments) {
        if ($Label -eq 'Discovery scan (tools\bt-scan.ps1)') {
            return "SCAN RESULT: le=12 classic=3 unpaired=7 (15s)`n"
        }
        & $savedWorker $Label $Action $Arguments
    }

    try { . $bridgeExecute } catch { . $bridgeCatch } finally { . $bridgeRestore }

    $hostText = $script:capturedHost -join "`n"
    if ($hostText -notmatch 'DISCOVERY VERDICT: PASS \(7 unpaired devices heard through this radio; AdvReports 5 -> 17\)') {
        throw "Discovery verdict PASS was not parsed correctly: $hostText"
    }
}


Write-Host 'Post-reboot radio-only restoration (in-memory devices and packages):'
$restoreAst = [Management.Automation.Language.Parser]::ParseFile((Join-Path $toolsDir 'uart-identify.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors) { throw ($parseErrors | Out-String) }
$restoreMode = $restoreAst.EndBlock.Statements | Where-Object {
    $_ -is [Management.Automation.Language.IfStatementAst] -and $_.Clauses[0].Item1.Extent.Text -eq '$RestoreOnly'
}
$restoreTry = $restoreMode.Clauses[0].Item2.Statements | Where-Object { $_ -is [Management.Automation.Language.TryStatementAst] }
$restoreExecute = [scriptblock]::Create(($restoreTry.Body.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")
$restoreCatch = [scriptblock]::Create(($restoreTry.CatchClauses[0].Body.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")
$restoreFinally = [scriptblock]::Create(($restoreTry.Finally.Statements | ForEach-Object { $_.Extent.Text }) -join "`n")

foreach ($case in @('RestoreClaim', 'RestoreDisabled', 'VendorAlreadyCurrent', 'AlreadyVendor', 'EnableVendor',
    'WrongRadio', 'ForeignOwner', 'ForeignPackage', 'MultiplePackages', 'SharedPackage',
    'BindingMismatch', 'ArmedProbe', 'ArmedIdentify', 'EnumerationFailure', 'QueryFailure',
    'StageFailure', 'DeleteTimeout', 'DeleteReboot', 'PackageSurvives', 'VendorInstallFailure',
    'WrongVendorRebind', 'WrongChildOwner', 'WrongChildIdentity', 'MissingChild', 'BadChildHealth', 'Cancel')) {
    Run-NamedScenario "RadioRestore $case" {
        foreach ($definition in $restoreAst.FindAll({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -in @('Get-Target', 'Invoke-Pnp', 'Get-RadioRestorePackages', 'Assert-RadioRestored', 'Invoke-RadioRestore')
        }, $true)) { . ([scriptblock]::Create($definition.Extent.Text)) }
        $expectedPlan = [pscustomobject]@{
            Radio = 'ACPI\QCOM2066\0'; Child = 'QCA_SHB\UART_H4\0'; VendorInf = 'mock-vendor\qcbtuart.inf'
        }
        $state = @{
            Owner = 'DeckBtUsb'; Radio = $expectedPlan.Radio; Disabled = $case -in @('RestoreDisabled', 'EnableVendor')
            PackagePresent = $true; OtherPackagePresent = $true; ProbeStarts = 0; Unsafe = $false
            Halted = $false; AccessAfterHalt = 0; Mutations = [System.Collections.Generic.List[string]]::new()
        }
        if ($case -in @('AlreadyVendor', 'EnableVendor')) { $state.Owner = 'QcBluetooth' }
        if ($case -eq 'ForeignOwner') { $state.Owner = 'ForeignDriver' }
        if ($case -eq 'WrongRadio') { $state.Radio = 'ACPI\QCOM2066\OTHER' }
        $KeepBound = $false; $DryRun = $false; $Force = $false
        $targetId = 'ACPI\QCOM2066'; $PnpTimeoutSeconds = 1; $paramsKey = 'Mock:\DeckBtUsb\Parameters'
        $LogPath = 'mock\restore-radio.log'
        $script:primaryExit = 0; $script:wedged = $false; $script:transcriptStarted = $false
        function Get-RadioRestorePlan { return $expectedPlan }
        function Test-DeckElevated { return $true }
        function New-Item { }
        function Start-Transcript { }
        function Stop-Transcript { }
        function Write-Bad([string] $Message) { Write-Host $Message }
        function Read-Host { if ($case -eq 'Cancel') { return 'NO' }; return 'RESTORE RADIO' }
        function Invoke-Bounded {
            param([string] $Label, [scriptblock] $Action, [object[]] $Arguments = @(), [int] $TimeoutSeconds)
            if ($state.Halted) { $state.AccessAfterHalt++; throw 'State access after a terminal native result.' }
            & $Action @Arguments
        }
        function Get-PnpDevice {
            [CmdletBinding()]
            param([string] $InstanceId, [switch] $PresentOnly)
            if ($case -eq 'QueryFailure') { throw 'PnP query failed.' }
            if ($state.Radio -like $InstanceId) {
                return [pscustomobject]@{
                    InstanceId = $state.Radio; Status = $(if ($state.Disabled) { 'Error' } else { 'OK' })
                    Problem = $(if ($state.Disabled) { 'CM_PROB_DISABLED' } else { 'CM_PROB_NONE' })
                }
            }
            if ($InstanceId -like 'QCA_SHB*' -and $state.Owner -eq 'QcBluetooth' -and $case -ne 'MissingChild') {
                return [pscustomobject]@{
                    InstanceId = $(if ($case -eq 'WrongChildIdentity') { 'QCA_SHB\UART_H4\OTHER' } else { $expectedPlan.Child })
                    Status = $(if ($case -eq 'BadChildHealth') { 'Error' } else { 'OK' })
                }
            }
        }
        function Get-PnpDeviceProperty {
            [CmdletBinding()]
            param([string] $InstanceId, [string] $KeyName)
            $data = switch ($KeyName) {
                'DEVPKEY_Device_Service' {
                    if ($InstanceId -eq $state.Radio) { $state.Owner }
                    elseif ($case -eq 'WrongChildOwner') { 'ForeignChild' } else { 'BthMini' }
                }
                'DEVPKEY_Device_DriverInfPath' { if ($case -eq 'BindingMismatch') { 'oem999.inf' } else { 'oem35.inf' } }
                'DEVPKEY_Device_ProblemStatus' { [uint32]0 }
                default { throw "Unexpected property: $KeyName" }
            }
            return [pscustomobject]@{ Data = $data }
        }
        function Get-CimInstance {
            [CmdletBinding()]
            param([string] $ClassName, [string] $Filter)
            [pscustomobject]@{ DeviceID = $state.Radio }
            if ($case -eq 'SharedPackage') { [pscustomobject]@{ DeviceID = 'ROOT\DEVGEN\DECKBTUSB' } }
        }
        function Test-Path {
            param([string] $LiteralPath)
            if ($LiteralPath -ne $paramsKey) { throw "Unexpected state path: $LiteralPath" }
            return $true
        }
        function Get-ItemProperty {
            [CmdletBinding()]
            param([string] $LiteralPath)
            return [pscustomobject]@{
                Enabled = [int]($case -eq 'ArmedProbe'); UartIdentify = [int]($case -eq 'ArmedIdentify')
                UartCompletion = 2 # Prior-boot terminal evidence is not a fresh probe.
            }
        }
        function Invoke-DeckNative {
            param([string] $Exe, [string[]] $Arguments)
            if ($Arguments[0] -eq '/enum-drivers') {
                if ($case -eq 'EnumerationFailure') { return [pscustomobject]@{ Code = 5; Output = '' } }
                $text = "Published Name: oem12.inf`nOriginal Name: isotest.inf`nProvider Name: DeckBtUsb`nDriver Version: 01/01/2026 1.0.0.0`n"
                if ($state.PackagePresent) {
                    $provider = if ($case -eq 'ForeignPackage') { 'ForeignProvider' } else { 'DeckBtUsb' }
                    $text += "Published Name: oem35.inf`nOriginal Name: deckbtusb.inf`nProvider Name: $provider`nDriver Version: 01/01/2026 1.0.0.0`n"
                }
                if ($case -eq 'MultiplePackages') {
                    $text += "Published Name: oem36.inf`nOriginal Name: deckbtusb.inf`nProvider Name: DeckBtUsb`nDriver Version: 01/01/2026 1.0.0.0`n"
                }
                return [pscustomobject]@{ Code = 0; Output = $text }
            }
            $state.Mutations.Add($Arguments -join ' ')
            switch ($Arguments[0]) {
                '/add-driver' {
                    if ($Arguments[1] -ne $expectedPlan.VendorInf) { $state.Unsafe = $true; throw 'Unknown vendor package.' }
                    if ($Arguments -contains '/install') {
                        if ($case -eq 'VendorInstallFailure') { return [pscustomobject]@{ Code = 5; Output = 'Install failed' } }
                        if ($case -ne 'WrongVendorRebind') { $state.Owner = 'QcBluetooth' }
                        if ($case -in @('VendorAlreadyCurrent', 'WrongVendorRebind')) {
                            return [pscustomobject]@{ Code = 259; Output = 'No driver installation performed' }
                        }
                    } elseif ($case -eq 'StageFailure') {
                        return [pscustomobject]@{ Code = 5; Output = 'Staging failed' }
                    }
                }
                '/delete-driver' {
                    if ($Arguments[1] -ne 'oem35.inf') { $state.OtherPackagePresent = $false; throw 'An unrelated package was removed.' }
                    if ($case -eq 'DeleteTimeout') {
                        $state.Halted = $true; $script:wedged = $true
                        throw 'Simulated bounded deletion timeout.'
                    }
                    if ($case -eq 'DeleteReboot') {
                        $state.Halted = $true
                        return [pscustomobject]@{ Code = 3010; Output = 'Reboot required' }
                    }
                    if ($case -ne 'PackageSurvives') { $state.PackagePresent = $false }
                }
                '/enable-device' {
                    if ($state.Owner -ne 'QcBluetooth') { $state.ProbeStarts++ }
                    $state.Disabled = $false
                }
                '/restart-device' { $state.ProbeStarts++ }
                default { $state.Unsafe = $true; throw 'Unexpected native mutation.' }
            }
            return [pscustomobject]@{ Code = 0; Output = 'Simulated operation complete' }
        }
        try { . $restoreExecute } catch { . $restoreCatch } finally { . $restoreFinally }
        $success = $case -in @('RestoreClaim', 'RestoreDisabled', 'VendorAlreadyCurrent', 'AlreadyVendor', 'EnableVendor')
        if ($script:primaryExit -ne [int](-not $success)) { throw "Unexpected recovery outcome: exit $($script:primaryExit)." }
        if (-not $state.OtherPackagePresent -or $state.Unsafe -or $state.ProbeStarts -ne 0) {
            throw 'Recovery changed unrelated state or started a probe.'
        }
        if ($state.AccessAfterHalt -ne 0) { throw 'Recovery accessed state after timeout/reboot-required.' }
        if ($success -and ($state.Owner -ne 'QcBluetooth' -or $state.Disabled)) { throw 'Success without an enabled vendor radio.' }
        if ($case -in @('RestoreClaim', 'RestoreDisabled', 'VendorAlreadyCurrent') -and $state.PackagePresent) { throw 'Claiming package survived successful restore.' }
        if ($case -eq 'AlreadyVendor' -and $state.Mutations.Count) { throw 'Already-healthy vendor state was mutated.' }
        if ($case -in @('WrongRadio', 'ForeignOwner', 'ForeignPackage', 'MultiplePackages', 'SharedPackage',
            'BindingMismatch', 'ArmedProbe', 'ArmedIdentify', 'EnumerationFailure', 'QueryFailure', 'Cancel') -and $state.Mutations.Count) {
            throw 'Refusal mutated device/package state.'
        }
        if ($case -eq 'StageFailure' -and -not $state.PackagePresent) { throw 'Removed the claiming package without a staged vendor backup.' }
    }
}

Write-Host ''
if ($null -ne $script:failedScenario) {
    Write-Host ("OPERATOR SELFTEST FAILED at scenario '{0}': {1}" -f $script:failedScenario, $script:failureError) -ForegroundColor Red
    exit 1
}

Write-Host ("OPERATOR SELFTEST PASSED: {0} scenarios passed" -f $script:scenarioCount) -ForegroundColor Green
exit 0
