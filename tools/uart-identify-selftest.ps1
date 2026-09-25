<#
.SYNOPSIS
    Exercises actual QCA UART identify routines against deterministic host I/O faults.
.DESCRIPTION
    No device, registry, or security changes. -Source accepts a previous driver source
    for regression reproduction; a failed scenario always returns a failing exit code.
#>
[CmdletBinding()]
param([string]$Source)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Source) { $Source = Join-Path $repo 'src\driver\qca_uart.c' }
$Source = (Resolve-Path -LiteralPath $Source).Path
$ewdk = 'C:\EWDK'
if ($env:EWDK) { $ewdk = $env:EWDK }
$msvc = (Get-ChildItem -LiteralPath (Join-Path $ewdk 'Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$sdk = Join-Path $ewdk 'Program Files\Windows Kits\10'
$sdkVer = '10.0.26100.0'
$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$scratch = Join-Path $PSScriptRoot ('_build\uart_identify_' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $scratch -Force

# Driver convention: return type and function name at column zero, final brace at
# column zero. Match definitions only, never a declaration or a call. Compilation
# validates the extracted bodies; a missing/ambiguous definition fails closed.
$text = [IO.File]::ReadAllText($Source)
$functionNames = @(
    'QcaUartRequestBudget',
    'QcaUartIsIoIdle',
    'QcaUartIoCompleted',
    'QcaUartSendTimed',
    'QcaUartPurgeTarget',
    'QcaUartCancelProbe',
    'QcaUartStopReadPump',
    'QcaUartStop',
    'QcaUartReleaseHardware',
    'QcaUartNoteAdvertiser',
    'QcaUartOnH4Packet',
    'QcaUartOnIbsByte',
    'QcaUartReadCompletion',
    'QcaUartStartReadPump',
    'QcaUartWriteSynchronous',
    'QcaUartSetHardwareFlow',
    'QcaUartGetModemStatus',
    'QcaUartSetManualRts',
    'QcaUartWakeController',
    'QcaUartPublishPhase',
    'QcaUartQuiesceRead',
    'QcaUartSwitchBaud',
    'QcaUartRunIdentify',
    'QcaUartResetSoc',
    'QcaUartProbeRate',
    'QcaUartEnsureRom',
    'QcaUartHandback',
    'QcaUartSnapshotSteady',
    'QcaUartServiceIbs',
    'QcaUartScoLoopPoll',
    'QcaUartScoLoopback',
    'QcaUartServeSteady',
    'QcaUartRequestStop'
)
$optionalNames = @('QcaUartIsIoIdle', 'QcaUartPurgeTarget', 'QcaUartQuiesceRead', 'QcaUartSwitchBaud',
    'QcaUartHandback', 'QcaUartResetSoc', 'QcaUartProbeRate', 'QcaUartEnsureRom',
    'QcaUartGetModemStatus', 'QcaUartSetManualRts', 'QcaUartWakeController', 'QcaUartPublishPhase')
$hasIsIoIdle = $false
$hasPurgeTarget = $false
$blocks = [System.Collections.Generic.List[string]]::new()
foreach ($name in $functionNames) {
    $pattern = '(?m)^(?:static )?(?:ULONG|VOID|void|BOOLEAN|NTSTATUS)\r?\n' +
        [regex]::Escape($name) + '\([^;]*?^\{\r?\n[\s\S]*?^\}'
    $matches = [regex]::Matches($text, $pattern)
    if ($matches.Count -eq 0 -and ($optionalNames -contains $name)) { continue }
    if ($matches.Count -ne 1) { throw "Expected one definition of $name in ${Source}; found $($matches.Count)" }
    if ($name -eq 'QcaUartIsIoIdle') { $hasIsIoIdle = $true }
    if ($name -eq 'QcaUartPurgeTarget') { $hasPurgeTarget = $true }
    $blocks.Add($matches[0].Value)
}

$driverSource = Join-Path (Split-Path $Source) 'driver.c'
if (Test-Path -LiteralPath $driverSource) {
    $driverText = [IO.File]::ReadAllText($driverSource)
    $driverPattern = '(?m)^(?:static )?(?:ULONG|VOID|void|BOOLEAN|NTSTATUS)\r?\n' +
        'DeckBtRecordProbeProgress\([^;]*?^\{\r?\n[\s\S]*?^\}'
    $driverMatches = [regex]::Matches($driverText, $driverPattern)
    if ($driverMatches.Count -eq 1) {
        $blocks.Add($driverMatches[0].Value)
    } elseif ($driverMatches.Count -gt 1) {
        throw "Expected one definition of DeckBtRecordProbeProgress in ${driverSource}; found $($driverMatches.Count)"
    }
}
$headerDefines = @()
if ($hasIsIoIdle) { $headerDefines += '#define HAVE_QCA_UART_IS_IO_IDLE 1' }
if ($hasPurgeTarget) { $headerDefines += '#define HAVE_QCA_UART_PURGE_TARGET 1' }
$extractedContent = ($headerDefines + $blocks) -join "`r`n`r`n"
[IO.File]::WriteAllText((Join-Path $scratch 'qca_uart_extracted.c'), $extractedContent)
$exe = Join-Path $scratch 'uart_identify_selftest.exe'
$clArgs = @('/nologo', '/W4', '/WX', "/Fe:`"$exe`"", "/Fo:`"$scratch/`"", "/I`"$scratch`"")
foreach ($inc in @(
    "$msvc\include", "$sdk\Include\$sdkVer\ucrt", "$sdk\Include\$sdkVer\shared",
    "$sdk\Include\$sdkVer\um", "$repo\src\include", "$repo\src\driver"
)) { $clArgs += "/I`"$inc`"" }
foreach ($file in @(
    'tools\uart_identify_selftest.c', 'src\common\qca_identify.c',
    'src\common\qca_tlv.c', 'src\common\h4_codec.c',
    'src\common\qca_init_fsm.c', 'src\common\hci_bridge.c', 'src\common\sco_route.c'
)) { $clArgs += ('"' + (Join-Path $repo $file) + '"') }
$clArgs += '/link'
foreach ($lib in @("$msvc\lib\x64", "$sdk\Lib\$sdkVer\ucrt\x64", "$sdk\Lib\$sdkVer\um\x64")) {
    $clArgs += "/LIBPATH:`"$lib`""
}
$compileLog = Join-Path $scratch 'compile.log'
$compileErr = Join-Path $scratch 'compile.err'
$compiler = Start-Process -FilePath $cl -ArgumentList $clArgs -NoNewWindow -PassThru -Wait -RedirectStandardOutput $compileLog -RedirectStandardError $compileErr
if ($compiler.ExitCode -ne 0) {
    Write-Host ([IO.File]::ReadAllText($compileLog) + [IO.File]::ReadAllText($compileErr))
    throw "UART harness compilation failed: $scratch"
}
Write-Host "UART identify source: $Source"
$runLog = Join-Path $scratch 'run.log'
$run = Start-Process -FilePath $exe -NoNewWindow -PassThru -RedirectStandardOutput $runLog
# Start-Process -PassThru reports a null ExitCode unless the handle is opened before exit;
# a null exit code became `exit 0`, so failing scenarios appeared to pass.
$null = $run.Handle
if (-not $run.WaitForExit(15000)) {
    $run.Kill()
    throw "UART harness exceeded 15-second host bound: $scratch"
}
$run.WaitForExit()
$runText = [IO.File]::ReadAllText($runLog)
Write-Host $runText
Write-Host "UART harness evidence: $scratch"
$summary = [regex]::Match($runText, 'UART IDENTIFY: (\d+)/(\d+) scenarios passed')
$allPassed = $summary.Success -and $summary.Groups[1].Value -eq $summary.Groups[2].Value -and
    -not ($runText -match '\[FAIL\]')
if ($run.ExitCode -ne 0 -or -not $allPassed) {
    Write-Host "UART IDENTIFY SELFTEST FAILED (exit=$($run.ExitCode))"
    exit 1
}
exit 0
