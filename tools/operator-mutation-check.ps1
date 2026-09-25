<#
    operator-mutation-check.ps1 - verifies service and operator test suites detect injected faults.

    Injects faults into temporary source copies and requires the corresponding tests to fail.
    Variants run in child processes; service scenarios use isolated HKCU test keys.
    Does not modify the working tree, production registry state, devices, or the radio.

    Two matrices:
      service - mutates deck-state.ps1, expects fewer service-selftest.ps1 checks to pass
      flow    - mutates uart-probe.ps1 / uart-identify.ps1, expects operator-selftest.ps1 to exit nonzero

    Exit 0 when every injected defect was caught; 1 when any survived or an anchor went stale
    (a stale anchor means the mutation was never applied, which is not a pass).
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$work = Join-Path ([IO.Path]::GetTempPath()) ('deckbt-mutation-{0}' -f ([guid]::NewGuid().ToString('N')))
$survivors = [System.Collections.Generic.List[string]]::new()
$applied = 0

$serviceSources = @('deck-state.ps1', 'service-selftest.ps1')
$flowSources = @('deck-state.ps1', 'uart-probe.ps1', 'uart-identify.ps1', 'uninstall.ps1', 'operator-selftest.ps1', 'bt-hid-watch.ps1', 'session.ps1')
if (Test-Path -LiteralPath (Join-Path $tools 'bt-scan.ps1')) {
    $flowSources += 'bt-scan.ps1'
}

function Get-Source([string] $Name) { return Get-Content -LiteralPath (Join-Path $tools $Name) -Raw }

function Invoke-Variant([string[]] $Sources, [string] $Entry, [string] $MutFile, [string] $MutText) {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    [void](New-Item -ItemType Directory -Path $work)
    foreach ($s in $Sources) {
        $content = if ($s -eq $MutFile) { $MutText } else { Get-Source $s }
        Set-Content -LiteralPath (Join-Path $work $s) -Value $content -Encoding UTF8
    }
    $p = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $work $Entry)) `
        -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $work 'out.txt')
    $out = if (Test-Path (Join-Path $work 'out.txt')) { Get-Content (Join-Path $work 'out.txt') -Raw } else { '' }
    $pass = if ($out -match '(\d+)\s+(?:checks|scenarios)\s+pass') { [int]$Matches[1] } else { -1 }
    return [pscustomobject]@{ Exit = $p.ExitCode; Pass = $pass; Output = $out }
}

$serviceMutations = @(
    @{ File = 'deck-state.ps1'; Name = 'unreadable key reported as absent'; Find = '-ErrorAction Stop'; Replace = '-ErrorAction SilentlyContinue' }
    @{ File = 'deck-state.ps1'; Name = 'radio guard accepts any owner'; Find = "    if (`$radioService -ne 'QcBluetooth' -or `$radio.Status -ne 'OK' -or"; Replace = '    if ($false -or' }
)

$flowMutations = @(
    @{ File = 'session.ps1'; Name = 'session accepts more than one mode'; Find = 'if ($modeCount -ne 1) {'; Replace = 'if ($modeCount -lt 1) {' }
    @{ File = 'uart-identify.ps1'; Name = 'identify ignores driver load failure'; Find = 'Assert-DeckNoDeviceProblem $node'; Replace = '# mutated' }
    @{ File = 'uart-probe.ps1'; Name = 'probe ignores driver load failure'; Find = 'Assert-DeckNoDeviceProblem $node'; Replace = '# mutated' }
    @{ File = 'uart-identify.ps1'; Name = 'identify stale baud overrides transport failure'; Find = '($values.UartFailurePhase -eq ''Answered'') -and ($hasLastStatus -and $lastStatusVal -eq 0)'; Replace = '$true' }
    @{ File = 'uart-identify.ps1'; Name = 'identify classifies transport error as silent'; Find = '$values.UartFailurePhase -eq ''NoResponse'' -and $hasLastStatus -and $lastStatusVal -eq 3221226021 -and $hasAborted -and $abortedVal -eq 0'; Replace = '($null -eq $values.UartIdentifyBaud -or $values.UartIdentifyBaud -eq 0)' }
    @{ File = 'uart-identify.ps1'; Name = 'identify incomplete status accepted as answered'; Find = '$hasLastStatus -and $lastStatusVal -eq 0'; Replace = '$lastStatusVal -eq 0' }
    @{ File = 'uart-identify.ps1'; Name = 'identify physical restore health guard dropped'; Find = "Invoke-Bounded 'Verify vendor radio health' { Assert-DeckHealthyVendorRadio } @() `$remaining"; Replace = '[pscustomobject]@{}' }
    @{ File = 'uart-probe.ps1'; Name = 'probe physical restore health guard dropped'; Find = "Invoke-Bounded 'Verify vendor radio health' { Assert-DeckHealthyVendorRadio } @() `$remaining"; Replace = '[pscustomobject]@{}' }
    @{ File = 'uart-identify.ps1'; Name = 'identify package cleanup deletes foreign packages'; Find = '$packages = @($packages | Where-Object { $_.Published -eq $script:stagedOemInf })'; Replace = '# mutated' }
    @{ File = 'uart-probe.ps1'; Name = 'probe package cleanup deletes foreign packages'; Find = '$packages = @($packages | Where-Object { $_.Published -eq $script:stagedOemInf })'; Replace = '# mutated' }
    @{ File = 'uart-identify.ps1'; Name = 'identify physical precondition accepts staged package'; Find = 'if ($existingPackages.Count) {'; Replace = 'if ($false) {' }
    @{ File = 'uart-probe.ps1'; Name = 'probe physical precondition accepts staged package'; Find = 'if ($existingPackages.Count) {'; Replace = 'if ($false) {' }
    @{ File = 'uart-identify.ps1'; Name = 'identify accepts completion marker 0'; Find = '$values.UartCompletion -eq 1'; Replace = '($values.UartCompletion -in @(0, 1))' }
    @{ File = 'uart-probe.ps1'; Name = 'probe accepts completion marker 0'; Find = '$values.UartCompletion -eq 1'; Replace = '($values.UartCompletion -in @(0, 1))' }
    @{ File = 'uart-identify.ps1'; Name = 'identify restarts its active probe'; Find = '[void](Invoke-Pnp @($startAction, $node.InstanceId))'; Replace = '[void](Invoke-Pnp @($startAction, $node.InstanceId)); [void](Invoke-Pnp @(''/restart-device'', $node.InstanceId))' }
    @{ File = 'uart-probe.ps1'; Name = 'probe restarts immediately after enabling'; Find = '[void](Invoke-Pnp @(''/enable-device'', $node.InstanceId))'; Replace = '[void](Invoke-Pnp @(''/enable-device'', $node.InstanceId)); [void](Invoke-Pnp @(''/restart-device'', $node.InstanceId))' }
    @{ File = 'uart-identify.ps1'; Name = 'radio restore removes a shared package'; Find = 'if ($users.Count -ne 1 -or $users[0].DeviceID -ine $Id) {'; Replace = 'if ($false) {' }
    @{ File = 'uart-identify.ps1'; Name = 'radio restore ignores armed tokens'; Find = '$value.Value -ne 0'; Replace = '$false' }
    @{ File = 'uart-identify.ps1'; Name = 'radio restore continues after reboot-required'; Find = 'if ($result.Code -eq 3010)'; Replace = 'if ($false)' }
    @{ File = 'uart-identify.ps1'; Name = 'radio restore omits final health verification'; Find = 'Assert-RadioRestored $Plan'; Replace = '$null' }
    @{ File = 'uart-identify.ps1'; Name = 'identify restore re-enables an enabled radio'; Find = '# Windows Home rejects enabling an enabled device (exit 50)'; Replace = '[void](Invoke-Pnp @(''/enable-device'', $node.InstanceId)) #' }
    @{ File = 'uart-probe.ps1'; Name = 'probe restore re-enables an enabled radio'; Find = '# Windows Home rejects enabling an enabled device (exit 50)'; Replace = '[void](Invoke-Pnp @(''/enable-device'', $node.InstanceId)) #' }
    @{ File = 'uart-probe.ps1'; Name = 'bridge unconfirmed release proceeds to vendor restore'; Find = 'if (-not $released) {'; Replace = 'if ($false) {' }
    @{ File = 'uart-probe.ps1'; Name = 'bridge leaves Enabled armed'; Find = "Invoke-Bounded 'Remove Enabled parameter' { param(`$Key) Remove-ItemProperty -LiteralPath `$Key -Name Enabled -ErrorAction SilentlyContinue } @(`$paramsKey)"; Replace = '# mutated' }
)

try {
    Write-Host 'Service contract matrix (gate: service-selftest.ps1)'
    $baseline = Invoke-Variant $serviceSources 'service-selftest.ps1' '' ''
    if ($baseline.Exit -ne 0) { throw "baseline service-selftest.ps1 does not pass (exit $($baseline.Exit)); fix that before mutation testing." }
    Write-Host ("  baseline: {0} checks pass" -f $baseline.Pass)
    foreach ($m in $serviceMutations) {
        $source = Get-Source $m.File
        if (-not $source.Contains($m.Find)) {
            Write-Host ("  [STALE] {0}: anchor no longer present in {1}" -f $m.Name, $m.File) -ForegroundColor Yellow
            $survivors.Add("$($m.Name) (stale anchor)")
            continue
        }
        $applied++
        $r = Invoke-Variant $serviceSources 'service-selftest.ps1' $m.File $source.Replace($m.Find, $m.Replace)
        if ($r.Pass -lt $baseline.Pass) {
            Write-Host ("  [killed] {0}: {1} -> {2} passing" -f $m.Name, $baseline.Pass, $r.Pass) -ForegroundColor DarkGreen
        } else {
            Write-Host ("  [SURVIVED] {0}: still {1} passing" -f $m.Name, $r.Pass) -ForegroundColor Red
            $survivors.Add($m.Name)
        }
    }

    Write-Host ''
    Write-Host 'Operator flow matrix (gate: operator-selftest.ps1)'
    $flowBase = Invoke-Variant $flowSources 'operator-selftest.ps1' '' ''
    if ($flowBase.Exit -ne 0) { throw "baseline operator-selftest.ps1 does not pass (exit $($flowBase.Exit)); fix that before mutation testing." }
    Write-Host ("  baseline: exit {0}, {1} scenarios pass" -f $flowBase.Exit, $flowBase.Pass)
    foreach ($m in $flowMutations) {
        $source = Get-Source $m.File
        if (-not $source.Contains($m.Find)) {
            Write-Host ("  [STALE] {0}: anchor no longer present in {1}" -f $m.Name, $m.File) -ForegroundColor Yellow
            $survivors.Add("$($m.Name) (stale anchor)")
            continue
        }
        $applied++
        $r = Invoke-Variant $flowSources 'operator-selftest.ps1' $m.File $source.Replace($m.Find, $m.Replace)
        if ($r.Exit -ne 0) {
            Write-Host ("  [killed] {0}: suite exit {1}" -f $m.Name, $r.Exit) -ForegroundColor DarkGreen
        } else {
            Write-Host ("  [SURVIVED] {0}: suite still passed" -f $m.Name) -ForegroundColor Red
            $survivors.Add($m.Name)
        }
    }
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}

Write-Host ''
if ($survivors.Count -eq 0) {
    Write-Host ("MUTATION CHECK PASSED: all {0} mutations caught" -f $applied) -ForegroundColor Green
    exit 0
}
Write-Host ("MUTATION CHECK FAILED: " + ($survivors -join '; ')) -ForegroundColor Red
exit 1
