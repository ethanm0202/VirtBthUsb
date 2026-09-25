<#
    qca-mutation-check.ps1 - verifies QCA response matching, identification, and NVM selection.

    Inject a known defect into a scratch copy of the source under test, rebuild the check against the
    mutated copy, and require it to fail. A mutation that still passes means the test suite does not
    catch that defect; a stale anchor means the mutation was never applied and is also reported as a failure.

    Picking the wrong NVM for this radio is the exact class of mistake that can leave the
    controller unusable, so these rules must not be able to rot silently.

    Nothing here touches the working tree, a device, the registry, or the radio: every variant is
    compiled in a temp directory and run there, with the repository root only as the working
    directory so the check can read the hash-verified vendor NVM copies.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$tools = $PSScriptRoot
$root = Split-Path -Parent $tools
$work = Join-Path ([IO.Path]::GetTempPath()) ('deckbt-qca-mutation-{0}' -f ([guid]::NewGuid().ToString('N')))

# Same toolchain discovery as tools/selftest.cmd; no installation, no elevation.
$ewdk = if ($env:EWDK) { $env:EWDK } else { 'C:\EWDK' }
# Firmware for the suites: same rule as tools/selftest.cmd.
if (-not $env:QCA_FW_DIR) {
    $fwRepo = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
    $fw = Get-ChildItem -LiteralPath $fwRepo -Directory -Filter 'qcbtuart.inf_amd64_*' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($fw) { $env:QCA_FW_DIR = $fw.FullName }
}
$msvcRoot = Join-Path $ewdk 'Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
$sdk = Join-Path $ewdk 'Program Files\Windows Kits\10'
$sdkVer = '10.0.26100.0'
$msvc = @(Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name)
if ($msvc.Count -eq 0) { throw "No MSVC toolset under $msvcRoot" }
$msvc = $msvc[-1].FullName
$cl = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path -LiteralPath $cl)) { throw "cl.exe not found at $cl" }
$include = @(
    (Join-Path $msvc 'include'),
    (Join-Path $sdk "Include\$sdkVer\ucrt"),
    (Join-Path $sdk "Include\$sdkVer\shared"),
    (Join-Path $sdk "Include\$sdkVer\um")
) -join ';'
$lib = @(
    (Join-Path $msvc 'lib\x64'),
    (Join-Path $sdk "Lib\$sdkVer\ucrt\x64"),
    (Join-Path $sdk "Lib\$sdkVer\um\x64")
) -join ';'

$sources = @(
    @{ Rel = 'src\include\qca_protocol.h' },
    @{ Rel = 'src\include\qca_init_fsm.h' },
    @{ Rel = 'src\common\qca_tlv.c' },
    @{ Rel = 'src\common\qca_init_fsm.c' },
    @{ Rel = 'tools\qca_fsm_selftest.c' },
    @{ Rel = 'tools\nvm_selftest.c' },
    @{ Rel = 'src\include\qca_identify.h' },
    @{ Rel = 'src\common\qca_identify.c' },
    @{ Rel = 'tools\identify_selftest.c' }
)

$mutations = @(
    @{ File = 'src\common\qca_tlv.c'; Name = '3.2 Mbaud support removed'
       Find = 'case 3200000u: *Index = QCA_BAUDRATE_3200000; return TRUE;'
       Replace = 'case 3200000u: *Index = 0; return FALSE;' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'foundry test inverted'
       Find = 'isGf = ((SocId & QCA_HSP_GF_SOC_MASK) == QCA_HSP_GF_SOC_ID);'
       Replace = 'isGf = ((SocId & QCA_HSP_GF_SOC_MASK) != QCA_HSP_GF_SOC_ID);' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'foundry test compares against an invalid constant'
       Find = 'isGf = ((SocId & QCA_HSP_GF_SOC_MASK) == QCA_HSP_GF_SOC_ID);'
       Replace = 'isGf = ((SocId & QCA_HSP_GF_SOC_MASK) == 0xdeadbeefu);' }
    @{ File = 'src\common\qca_tlv.c'; Name = '0xffff no longer means "no board"'
       Find = 'hasBoard = (BoardId != 0x0000u && BoardId != 0xFFFFu);'
       Replace = 'hasBoard = (BoardId != 0x0000u);' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'board id byte order swapped'
       Find = '*BoardId = (USHORT)(((USHORT)data[1] << 8) | (USHORT)data[2]);'
       Replace = '*BoardId = (USHORT)(((USHORT)data[2] << 8) | (USHORT)data[1]);' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'board id request skipped entirely'
       Find = 'Fsm->State = QcaFsmStateBoardIdRequest;'
       Replace = 'Fsm->State = QcaFsmStateNvmDownload;' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'unanswered board id reported as valid'
       Find = 'Fsm->BoardIdValid = FALSE;'
       Replace = 'Fsm->BoardIdValid = TRUE;' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'nvm fallback no longer recorded'
       Find = 'Fsm->NvmFallbackUsed = TRUE;'
       Replace = 'Fsm->NvmFallbackUsed = FALSE;' }
    @{ File = 'src\common\qca_identify.c'; Name = 'identify emits a TLV-download sub-op instead of the version request'
       Find = '    Out[4] = EDL_PATCH_VER_REQ_CMD;'
       Replace = '    Out[4] = EDL_PATCH_TLV_REQ_CMD;' }
    @{ File = 'src\common\qca_identify.c'; Name = 'identify ladder drops the vendor 3.2 Mbaud rung'
       Find = '    Id->Rates[2] = 3200000ul;'
       Replace = '    Id->Rates[2] = 921600ul;' }
    @{ File = 'src\common\qca_identify.c'; Name = 'identify accepts any packet as an identification'
       Find = '    if (!QcaParseVersionEvent(Packet, Length, &ver)) {'
       Replace = '    if (FALSE) {' }
    @{ File = 'src\common\qca_identify.c'; Name = 'identify ladder loses its bound'
       Find = '    if (Id->Index >= Id->RateCount) {'
       Replace = '    if (FALSE) {' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'patch download falls through to board ID'
       Find = '        break; /* Patch download must not fall through to board-ID parsing. */'
       Replace = '' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'disable logging accepts unrelated or failed completion'
       Find = 'opcode == QCA_DISABLE_LOGGING && status == 0 &&'
       Replace = '' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'disable logging rejects the QCA2066 sub-op echo'
       Find = '(Length == 7u || (Length == 8u && Packet[7] == QCA_DISABLE_LOGGING_SUB_OP))'
       Replace = 'Length == 7u' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'build info accepts an arbitrary command completion'
       Find = 'if (IsBuildInfoResponse(Packet, Length)) {'
       Replace = 'if (IsBuildInfoResponse(Packet, Length) || IsCommandComplete(Packet, Length, &opcode, &status)) {' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'EDL accepts bytes beyond its declared event length'
       Find = 'Length != 3u + (ULONG)Packet[2]'
       Replace = 'Length < 3u + (ULONG)Packet[2]' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'QCA2066 Command Complete envelope not recognized'
       Find = 'Packet[1] == 0x0Eu && Length >= 8u'
       Replace = 'Packet[1] == 0x0Fu && Length >= 8u' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'failed TLV result accepted'
       Find = 'dataLength == 1u && data[0] == 0'
       Replace = 'dataLength == 1u' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'disable logging omits its enable parameter'
       Find = 'Out[3] = 2u;'
       Replace = 'Out[3] = 1u;' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'NVM baud byte not patched on the wire'
       Find = 'Buffer[6u + (Fsm->NvmBaudOffset - segStart)] = Fsm->OperBaudIndex;'
       Replace = '(void)segStart;' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'FSM accepts an NVM without an HCI tag'
       Find = '!QcaFindNvmHciBaudOffset(Candidates[i].Data, Candidates[i].Size, &baudOffset)) {'
       Replace = '(QcaFindNvmHciBaudOffset(Candidates[i].Data, Candidates[i].Size, &baudOffset) && FALSE)) {' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'NVM tag walk ignores tag length bounds'
       Find = 'if (info.Length - idx - QCA_NVM_TAG_HDR_SIZE < tagLen) {'
       Replace = 'if (FALSE) {' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'NVM HCI tag shorter than 3 bytes accepted'
       Find = 'if (tagLen < 3u) {'
       Replace = 'if (FALSE) {' }
    @{ File = 'src\common\qca_tlv.c'; Name = 'NVM baud patch lands on HCI data[0]'
       Find = '*Offset = 4u + idx + QCA_NVM_TAG_HDR_SIZE + 1u;'
       Replace = '*Offset = 4u + idx + QCA_NVM_TAG_HDR_SIZE;' }
    @{ File = 'src\common\qca_init_fsm.c'; Name = 'FSM waits for a 0xFC48 reply this controller never sends'
       Find = "        Fsm->State = QcaFsmStatePatchDownload;`n        return QcaFsmActionSendBaudAndSwitch;"
       Replace = "        return QcaFsmActionSendBaudAndSwitch;" }

# Note: "stops probing after an answer" is guarded twice (Id->Answered and Id->Done, both set by
# QcaIdentifyOnPacket), so no single-line mutation can defeat it. That is defence in depth, not a
# gap in test coverage: tools/identify_selftest.c asserts the behaviour directly.
)

function Invoke-Check([string] $MutFile, [string] $MutText) {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    foreach ($dir in @('src\include', 'src\common', 'tools', 'obj')) {
        [void](New-Item -ItemType Directory -Path (Join-Path $work $dir) -Force)
    }
    foreach ($s in $sources) {
        $text = if ($s.Rel -eq $MutFile) { $MutText } else { Get-Content -LiteralPath (Join-Path $root $s.Rel) -Raw }
        Set-Content -LiteralPath (Join-Path $work $s.Rel) -Value $text -Encoding UTF8 -NoNewline
    }
    # All three C test suites run; a mutation is caught when any stops passing.
    $env:INCLUDE = $include
    $env:LIB = $lib
    $totalPass = 0
    foreach ($suite in @(
        @{ Entry = 'tools\nvm_selftest.c'; Deps = @('src\common\qca_tlv.c', 'src\common\qca_init_fsm.c'); Exe = 'nvm_selftest.exe' },
        @{ Entry = 'tools\identify_selftest.c'; Deps = @('src\common\qca_identify.c', 'src\common\qca_tlv.c'); Exe = 'identify_selftest.exe' },
        @{ Entry = 'tools\qca_fsm_selftest.c'; Deps = @('src\common\qca_init_fsm.c', 'src\common\qca_tlv.c'); Exe = 'qca_fsm_selftest.exe' }
    )) {
        $exe = Join-Path $work $suite.Exe
        $args = @('/nologo', '/W4', '/WX', ('/Fe:' + $exe), ('/Fo:' + (Join-Path $work 'obj\')),
                  (Join-Path $work $suite.Entry))
        foreach ($d in $suite.Deps) { $args += (Join-Path $work $d) }
        $build = & $cl @args 2>&1
        if ($LASTEXITCODE -ne 0) {
            return [pscustomobject]@{ Built = $false; Exit = $LASTEXITCODE; Output = ($build | Out-String) }
        }
        Push-Location $root
        try {
            $out = & $exe 2>&1
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        if ($code -ne 0) {
            return [pscustomobject]@{ Built = $true; Exit = $code; Output = ($out | Out-String) }
        }
        $totalPass += ([regex]::Matches(($out | Out-String), '\[pass\]')).Count
    }
    return [pscustomobject]@{ Built = $true; Exit = 0; Output = ''; Pass = $totalPass }
}

$survivors = [System.Collections.Generic.List[string]]::new()
$applied = 0
try {
    $baseline = Invoke-Check '' ''
    if (-not $baseline.Built) { throw "baseline build failed:`n$($baseline.Output)" }
    if ($baseline.Exit -ne 0) { throw "baseline QCA checks do not pass:`n$($baseline.Output)" }
    Write-Host ("baseline: NVM, identify, and FSM checks exit {0}" -f $baseline.Exit)

    foreach ($m in $mutations) {
        $source = Get-Content -LiteralPath (Join-Path $root $m.File) -Raw
        if (-not $source.Contains($m.Find)) {
            Write-Host ("  [STALE] {0}: anchor no longer present in {1}" -f $m.Name, $m.File) -ForegroundColor Yellow
            $survivors.Add("$($m.Name) (stale anchor)")
            continue
        }
        $applied++
        $r = Invoke-Check $m.File $source.Replace($m.Find, $m.Replace)
        if (-not $r.Built) {
            # A mutation that will not compile is still caught: the defect cannot ship.
            Write-Host ("  [killed] {0}: rejected at compile time" -f $m.Name) -ForegroundColor DarkGreen
        } elseif ($r.Exit -ne 0) {
            Write-Host ("  [killed] {0}: exit {1}" -f $m.Name, $r.Exit) -ForegroundColor DarkGreen
        } else {
            Write-Host ("  [SURVIVED] {0}: check still passed" -f $m.Name) -ForegroundColor Red
            $survivors.Add($m.Name)
        }
    }

    # Driver-side identify rules run in the UART harness against a mutated driver copy. The
    # CTS guard ensures writes are not attempted while CTS is deasserted.
    $uartHarness = Join-Path $tools 'uart-identify-selftest.ps1'
    $driverSource = Join-Path $root 'src\driver\qca_uart.c'
    $driverMutations = @(
        @{ Name = 'identify transmits without the CTS guard'
           Find = "        status = QcaUartWakeController(Uart, Record);`n        if (!NT_SUCCESS(status)) {`n            failureStep = Record->LastStep;"
           Replace = "        status = STATUS_SUCCESS;`n        if (!NT_SUCCESS(status)) {`n            failureStep = Record->LastStep;" }
        @{ Name = 'wake pulses RTS without manual RTS control'
           Find = '    status = QcaUartSetManualRts(Uart);'
           Replace = '    status = STATUS_SUCCESS;' }
        @{ Name = 'wake reports CTS without reading it'
           Find = '    Record->CtsAsserted = (NT_SUCCESS(status) && (modem & SERIAL_CTS_STATE) != 0) ? 1u : 0u;'
           Replace = '    Record->CtsAsserted = NT_SUCCESS(status) ? 1u : 0u;' }
        @{ Name = 'wake leaves RTS under manual control'
           Find = '        restoreStatus = QcaUartSetHardwareFlow(Uart, TRUE);'
           Replace = '        restoreStatus = STATUS_SUCCESS;' }
        @{ Name = 'unreadable modem status treated as CTS present'
           Find = "    if (!NT_SUCCESS(status)) {`n        Record->CtsAsserted = 0;"
           Replace = "    if (FALSE) {`n        Record->CtsAsserted = 0;" }
        @{ Name = 'baud switch reconfigures under an active reader'
           Find = "    status = QcaUartQuiesceRead(Uart);`n    if (!NT_SUCCESS(status)) {`n        return status;`n    }`n    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_BAUD_SETTLE_MS);"
           Replace = "    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_BAUD_SETTLE_MS);" }
        @{ Name = 'host switches before the controller settles'
           Find = "    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);`n    if (QcaUartRequestBudget(Uart, 1) == 0) {`n        return STATUS_CANCELLED;`n    }`n    status = QcaUartSetBaudRate(Uart, BaudRate);"
           Replace = "    (void)delay;`n    status = QcaUartSetBaudRate(Uart, BaudRate);" }
        @{ Name = 'baud switch keeps bytes received across the rate change'
           Find = "    status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_PURGE, &purgeMask, sizeof(purgeMask), NULL, 0);`n    if (!NT_SUCCESS(status)) {`n        return status;`n    }`n    return QcaUartStartReadPump(Uart);"
           Replace = "    (void)purgeMask;`n    return QcaUartStartReadPump(Uart);" }
        @{ Name = 'reader not restarted after the baud switch'
           Find = '    return QcaUartStartReadPump(Uart);'
           Replace = '    return STATUS_SUCCESS;' }
        @{ Name = 'a rate the host UART refuses aborts the identify ladder'
           Find = '        if (status == STATUS_INVALID_PARAMETER || status == STATUS_NOT_SUPPORTED) {'
           Replace = '        if (FALSE) {' }
        @{ Name = 'SoC reset sends the wrong reset opcode'
           Find = '    static const UCHAR socReset[] = { H4_PKT_COMMAND, 0x40, 0xFC, 0x00 };'
           Replace = '    static const UCHAR socReset[] = { H4_PKT_COMMAND, 0x00, 0xFC, 0x00 };' }
        @{ Name = 'SoC reset hits a sleeping controller without IBS wake'
           Find = "        status = QcaUartWriteSynchronous(Uart, ibsWake, sizeof(ibsWake));"
           Replace = "        (void)ibsWake;" }
        @{ Name = 'SoC reset transmits without the CTS guard'
           Find = "        status = QcaUartWakeController(Uart, Scratch);"
           Replace = "        status = STATUS_SUCCESS; (void)Scratch;" }
        @{ Name = 'bring-up trusts a patch left at the operating rate'
           Find = '        if (baud == QCA_INIT_BAUD_RATE) {'
           Replace = '        if (baud != 0) {' }
        @{ Name = 'bring-up resets a silent controller at the ROM rate'
           Find = 'baud != 0 ? baud : QCA_OPER_BAUD_RATE'
           Replace = 'baud != 0 ? baud : QCA_INIT_BAUD_RATE' }
        @{ Name = 'bring-up accepts a reset that did not take'
           Find = '                if (NT_SUCCESS(status) && baud != QCA_INIT_BAUD_RATE) {'
           Replace = '                if (FALSE) {' }
        @{ Name = 'rate probe leaks identify mode into the firmware probe'
           Find = '    Uart->ProbeMode = mode;'
           Replace = '    (void)mode;' }
        @{ Name = 'handback overwrites the probe verdict phase'
           Find = '    RtlCopyMemory(Record->FailurePhase, savedPhase, sizeof(savedPhase));'
           Replace = '    (void)savedPhase;' }
        # Steady state: IBS, readiness order, idle line, graceful stop.
        @{ Name = 'controller wake never acknowledged'
           Find = '        status = QcaUartWriteSynchronous(Uart, wakeAck, sizeof(wakeAck));'
           Replace = '        (void)wakeAck;' }
        @{ Name = 'controller wake not handed to the worker'
           Find = '        InterlockedExchange(&uart->IbsAckPending, 1);'
           Replace = '        (void)0;' }
        @{ Name = 'wake ack written into a deasserted CTS'
           Find = '    if (NT_SUCCESS(status) && (modem & SERIAL_CTS_STATE) == 0) {'
           Replace = '    if (FALSE) {' }
        @{ Name = 'serving loop ignores controller wakes'
           Find = "            if (waitStatus == STATUS_WAIT_2) {`n                QcaUartServiceIbs(Uart, record);"
           Replace = "            if (waitStatus == STATUS_WAIT_2) {`n                (void)0;" }
        @{ Name = 'steady event trace never captured'
           Find = '            uart->EventTraceCount++;'
           Replace = '            (void)slot;' }
        @{ Name = 'advertising reports flush the event trace'
           Find = '                uart->AdvReports++;'
           Replace = '                uart->EventTraceCount++;' }
        @{ Name = 'extended advertiser address read at the legacy offset'
           Find = '        address = &Payload[7];'
           Replace = '        address = &Payload[6];' }
        @{ Name = 'LE host-support rejection passed to BTHPORT'
           Find = '                fixed[5] = 0x00;'
           Replace = '                fixed[5] = HCI_ERR_UNSUPPORTED_FEATURE;' }
        @{ Name = 'every 0x11 completion rewritten to success'
           Find = '                Payload[3] == (UCHAR)(HCI_OP_WRITE_LE_HOST_SUPPORTED & 0xFFu) &&'
           Replace = '                TRUE &&' }
        @{ Name = 'steady reads still wait for a silent gap'
           Find = '        timeouts.ReadTotalTimeoutMultiplier = MAXULONG;'
           Replace = '        timeouts.ReadTotalTimeoutMultiplier = 0;' }
        @{ Name = 'steady read policy set under an active reader'
           Find = "    status = QcaUartQuiesceRead(Uart);`n    if (NT_SUCCESS(status)) {`n        SERIAL_TIMEOUTS timeouts;"
           Replace = "    status = STATUS_SUCCESS;`n    if (NT_SUCCESS(status)) {`n        SERIAL_TIMEOUTS timeouts;" }
        @{ Name = 'host wake skipped before the bridge opens'
           Find = '        status = QcaUartWriteSynchronous(Uart, wakeInd, sizeof(wakeInd));'
           Replace = '        status = STATUS_SUCCESS; (void)wakeInd;' }
        @{ Name = 'USB child published before the bridge answers'
           Find = '        HciBridgeSetReady(&Uart->Bridge, 1);'
           Replace = '        (void)0;' }
        @{ Name = 'steady reads keep the bring-up timeout'
           Find = '    if (!Read || InterlockedCompareExchange(&Uart->BudgetUnbounded, 0, 0) == 0) {'
           Replace = '    if (!Read || TRUE) {' }
        @{ Name = 'an idle line ends the steady reader'
           Find = '        (status == STATUS_CANCELLED && InterlockedCompareExchange(&uart->BudgetUnbounded, 0, 0) == 0)) {'
           Replace = '        status == STATUS_CANCELLED) {' }
        @{ Name = 'steady session terminated by the 60 s bring-up deadline'
           Find = "    if (InterlockedCompareExchange(&Uart->ProbeActive, 0, 0) == 0 ||`n        InterlockedCompareExchange(&Uart->BudgetUnbounded, 0, 0) != 0) {"
           Replace = "    if (InterlockedCompareExchange(&Uart->ProbeActive, 0, 0) == 0) {" }
        @{ Name = 'graceful stop skips the handback'
           Find = '        QcaUartHandback(Uart, record);'
           Replace = '        (void)record;' }
        @{ Name = 'stop request hard-cancels a serving session'
           Find = '    if (Uart->ProbeMode != QcaProbeModeSteady || KeReadStateEvent(&Uart->SteadyReady) == 0) {'
           Replace = '    if (TRUE) {' }
        @{ Name = 'overrunning handback never escalated'
           Find = '    if (KeWaitForSingleObject(&Uart->SteadyStopped, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS) {'
           Replace = '    if (((void)timeout, FALSE)) {' }
    )
    $driverText = [IO.File]::ReadAllText($driverSource)
    $mutDir = Join-Path $work 'driver'
    foreach ($m in $driverMutations) {
        if (-not $driverText.Contains($m.Find)) {
            Write-Host ("  [STALE] {0}: anchor no longer present in qca_uart.c" -f $m.Name) -ForegroundColor Yellow
            $survivors.Add("$($m.Name) (stale anchor)")
            continue
        }
        $applied++
        [void](New-Item -ItemType Directory -Path $mutDir -Force)
        Copy-Item -LiteralPath (Join-Path $root 'src\driver\driver.c') -Destination $mutDir -Force
        $mutFile = Join-Path $mutDir 'qca_uart.c'
        [IO.File]::WriteAllText($mutFile, $driverText.Replace($m.Find, $m.Replace))
        $null = & powershell -NoProfile -ExecutionPolicy Bypass -File $uartHarness -Source $mutFile 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host ("  [killed] {0}: UART harness exit {1}" -f $m.Name, $LASTEXITCODE) -ForegroundColor DarkGreen
        } else {
            Write-Host ("  [SURVIVED] {0}: UART harness still passed" -f $m.Name) -ForegroundColor Red
            $survivors.Add($m.Name)
        }
    }
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}

Write-Host ''
if ($survivors.Count -eq 0) {
    Write-Host ("QCA MUTATION CHECK PASSED: {0} injected defects, all caught." -f $applied) -ForegroundColor Green
    exit 0
}
Write-Host ('QCA MUTATION CHECK FAILED: ' + ($survivors -join '; ')) -ForegroundColor Red
exit 1
