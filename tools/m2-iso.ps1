[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [ValidateSet('Observe', 'Synthesize')]
    [string] $IsochClock = 'Observe',
    [string] $ReportPath,
    [switch] $DryRun,
    [switch] $Rollback
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$package = Join-Path $root "src\isotest\x64\$Configuration\isotest"
$inf = Join-Path $package 'isotest.inf'
$sys = Join-Path $package 'isotest.sys'
$fltSys = Join-Path $package 'isoflt.sys'
$cat = Join-Path $package 'isotest.cat'
$shippingInf = Join-Path $root "src\filter\x64\$Configuration\deckbtflt\deckbtflt.inf"
$harness = Join-Path $root 'tools\_build\isotest\isotest.exe'
$modeTag = $IsochClock.ToLowerInvariant()
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path (Split-Path -Parent $harness) "report-$modeTag-$timestamp.csv"
}
$fullReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$devGen = 'C:\EWDK\Program Files\Windows Kits\10\Tools\10.0.26100.0\x64\devgen.exe'
$hardwareId = 'root\DeckBtIsoTest'
$instanceId = 'ROOT\DEVGEN\DECKBTISOTEST'
$childHardwareId = 'USB\VID_CAFE&PID_4001'
$createdByRun = $false
$primaryExit = 0
$logPath = Join-Path (Split-Path -Parent $harness) ("run-{0}-{1}-{2}.log" -f $modeTag, $timestamp, $PID)
$fullLogPath = [System.IO.Path]::GetFullPath($logPath)
$transcriptStarted = $false
$harnessBuildStamp = 'not reported'

function Write-Step([string] $Message) { Write-Host "[*] $Message" -ForegroundColor Cyan }
function Write-Ok([string] $Message) { Write-Host "[+] $Message" -ForegroundColor Green }
function Write-Warn2([string] $Message) { Write-Host "[!] $Message" -ForegroundColor Yellow }

function Invoke-CheckedNative {
    param(
        [string] $Executable,
        [string[]] $Arguments,
        [int[]] $SuccessCodes = @(0),
        [System.Collections.Generic.List[string]] $OutputList = $null
    )
    Write-Step ("{0} {1}" -f $Executable, ($Arguments -join ' '))
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($null -ne $OutputList) {
            & $Executable @Arguments 2>&1 | ForEach-Object {
                $line = $_.ToString()
                $OutputList.Add($line)
                Write-Host $line
            }
        } else {
            & $Executable @Arguments 2>&1 | Out-Host
        }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($SuccessCodes -notcontains $code) {
        throw "$Executable failed with exit code $code"
    }
    return $code
}

function Confirm-HarnessBuildStamp([System.Collections.Generic.List[string]] $OutputLines) {
    $stamp = $null
    if ($null -ne $OutputLines) {
        foreach ($line in $OutputLines) {
            if ($line -match '^\s*\[\*\]\s+Harness build:\s*(.+)$') {
                $stamp = $Matches[1].Trim()
                break
            }
        }
    }
    if ($stamp) {
        Write-Host "Harness build confirmed: $stamp"
        return $stamp
    } else {
        Write-Host 'Harness build stamp not reported - the harness predates build stamping; rebuild with tools\M2-ISO-BUILD.cmd before trusting this report.'
        return 'not reported'
    }
}

function Show-RunIdentity {
    Write-Host ""
    Write-Host "RUN IDENTITY"
    Write-Host "  Clock mode:     $IsochClock"
    Write-Host "  Report:         $fullReportPath"
    Write-Host "  Log:            $fullLogPath"
    if (Test-Path -LiteralPath $harness -PathType Leaf) {
        $hItem = Get-Item -LiteralPath $harness
        Write-Host "  Harness:        $($hItem.FullName)"
        Write-Host "    Last write:   $($hItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
        Write-Host "    Size:         $($hItem.Length) bytes"
    } else {
        Write-Host "  Harness:        $harness (missing - file not found)"
    }
    if (Test-Path -LiteralPath $sys -PathType Leaf) {
        $sItem = Get-Item -LiteralPath $sys
        Write-Host "  isotest.sys:    $($sItem.FullName)"
        Write-Host "    Last write:   $($sItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
    } else {
        Write-Host "  isotest.sys:    $sys (missing - file not found)"
    }
    Write-Host ""
}

function Assert-Elevated {
    if ($DryRun) { return }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
        throw 'Live measurement/rollback requires elevation. Run tools\M2-ISO-MEASURE.cmd.'
    }
}

function Get-InstrumentDevice {
    return @(Get-PnpDevice -PresentOnly -ErrorAction Stop |
        Where-Object { $_.InstanceId -eq $instanceId })
}

function Assert-PackageContract {
    foreach ($path in @($inf, $sys, $fltSys, $cat)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required built package file is missing: $path. Run tools\M2-ISO-BUILD.cmd $Configuration first."
        }
    }
    if (-not (Test-Path -LiteralPath $harness -PathType Leaf)) {
        throw "Measurement harness is missing: $harness. Run tools\M2-ISO-BUILD.cmd $Configuration first."
    }

    # Read the generated INF rather than assuming the source template's identities.
    $text = Get-Content -LiteralPath $inf -Raw
    if ($text -notmatch '(?im)^\s*[^;\r\n]+\s*=\s*[^,\r\n]+,\s*root\\DeckBtIsoTest\s*$') {
        throw "Generated INF does not contain the required root binding ${hardwareId}: $inf"
    }
    if ($text -notmatch '(?im)^\s*[^;\r\n]+\s*=\s*[^,\r\n]+,\s*USB\\VID_CAFE&PID_4001\s*$') {
        throw "Generated INF does not contain the required child binding ${childHardwareId}: $inf"
    }
    if ($text -notmatch '(?im)^\s*Include\s*=\s*winusb\.inf\s*$' -or
        $text -notmatch '(?im)^\s*Needs\s*=\s*WINUSB\.NT(?:\.Services)?\s*$') {
        throw "Generated INF does not declare the WinUSB package prerequisites: $inf"
    }
    if ($text -notmatch '(?im)^\s*HKR\s*,\s*,\s*"LowerFilters"\s*,\s*(?:0x00010008|65544)\s*,\s*"DeckBtIsoFlt"\s*$') {
        throw "Generated INF does not declare the required DeckBtIsoFlt lower filter on the child: $inf"
    }
    if ($text -notmatch '(?im)^\s*HKLM\s*,\s*"SYSTEM\\CurrentControlSet\\Services\\DeckBtIsoFlt\\Parameters"\s*,\s*"IsochClockMode"\s*,\s*(?:0x00010003|65539)\s*,\s*1\s*$') {
        throw "Generated INF must seed IsochClockMode with FLG_ADDREG_NOCLOBBER so install cannot overwrite the selected mode: $inf"
    }
    Write-Ok "Verified generated INF root binding ($hardwareId), child WinUSB binding ($childHardwareId), lower filter (DeckBtIsoFlt), and seeded IsochClockMode."
}

function Show-FilterDiagnostics {
    $filterParamsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoFlt\Parameters'
    Write-Step "Reading filter diagnostics from $filterParamsKey..."
    if (-not (Test-Path -LiteralPath $filterParamsKey)) {
        Write-Warn2 "Filter parameters key is absent: $filterParamsKey"
        return
    }
    $item = Get-Item -LiteralPath $filterParamsKey
    $names = @($item.GetValueNames())

    $ntStatusNames = @(
        'UsbdiLastStatus',
        'QueryBusTimeLastStatus',
        'QueryBusTimeUnderlyingStatus',
        'QueryBusTimeProbeStatus',
        'QueryBusTimeExProbeStatus'
    )
    $targetValues = @(
        'UsbdiQueryCount',
        'UsbdiLastRequestedSize',
        'UsbdiLastRequestedVersion',
        'UsbdiLastStatus',
        'QueryBusTimeCalls',
        'QueryBusTimeExCalls',
        'QueryBusTimeLastStatus',
        'QueryBusTimeLastFrame',
        'QueryBusTimeUnderlyingStatus',
        'QueryBusTimeProbeStatus',
        'QueryBusTimeExProbeStatus',
        'IsochHookInstalled',
        'IsochHookSynthesizing',
        'IsochClockModeActive'
    )

    foreach ($name in $targetValues) {
        if ($names -contains $name) {
            $raw = $item.GetValue($name)
            if ($ntStatusNames -contains $name) {
                if (($name -eq 'QueryBusTimeProbeStatus' -or $name -eq 'QueryBusTimeExProbeStatus') -and ([int32]$raw -eq 0x00000103)) {
                    Write-Host ("  {0,-30} = not probed" -f $name)
                } else {
                    $formatted = ("0x{0:X8}" -f [int32]$raw)
                    Write-Host ("  {0,-30} = {1}" -f $name, $formatted)
                }
            } else {
                Write-Host ("  {0,-30} = {1}" -f $name, $raw)
            }
        } else {
            Write-Host ("  {0,-30} = absent" -f $name)
        }
    }

    $hasSynth = $names -contains 'IsochHookSynthesizing'
    $hasProbe = $names -contains 'QueryBusTimeProbeStatus'

    if (-not $hasProbe -and -not $hasSynth) {
        Write-Host "Verdict: QueryBusTimeProbeStatus and IsochHookSynthesizing are absent; cannot determine bus clock status."
    } elseif ($hasSynth -and [int32]$item.GetValue('IsochHookSynthesizing') -eq 1) {
        Write-Host "Verdict: The synthetic clock was serving calls during this run; a passing geometry matrix is therefore attributable to the substituted clock."
    } elseif (-not $hasProbe) {
        Write-Host "Verdict: QueryBusTimeProbeStatus is absent; cannot determine bus clock status."
    } else {
        $probeStatus = [int32]$item.GetValue('QueryBusTimeProbeStatus')
        if ($probeStatus -eq [int32]0xC00000BB) {
            Write-Host "Verdict: The underlying bus clock is unsupported. Rerun with -IsochClock Synthesize to test the fix."
        } elseif ($probeStatus -eq 0x00000103) {
            Write-Host "Verdict: No probe result was recorded."
        } elseif ($probeStatus -eq 0x00000000) {
            Write-Host "Verdict: The underlying clock works; the isochronous refusal must have another cause."
        } else {
            Write-Host ("Verdict: QueryBusTimeProbeStatus reported unexpected status 0x{0:X8}." -f $probeStatus)
        }
    }
}

function Show-DriverDiagnostics {
    $driverParamsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoTest\Parameters'
    Write-Step "Reading driver diagnostics from $driverParamsKey..."
    if (-not (Test-Path -LiteralPath $driverParamsKey)) {
        Write-Warn2 "Driver parameters key is absent: $driverParamsKey"
        return
    }
    $item = Get-Item -LiteralPath $driverParamsKey
    $names = @($item.GetValueNames())

    $ntStatusNames = @(
        'LastAddDeviceStatus',
        'LastIsochUsbdStatus'
    )
    $targetValues = @(
        'LastAddDeviceStep',
        'LastAddDeviceStatus',
        'EndpointsConfigureCount',
        'LastConfigureType',
        'LastConfigureInterface',
        'LastConfigureSetting',
        'CurrentAltSetting',
        'HoldCommandCount',
        'HeldTransferCount',
        'CancelCallbackCount',
        'ReleaseCommandCount',
        'HeldRequestActive',
        'IsochUrbCount',
        'LastIsochEndpoint',
        'LastIsochNumPackets',
        'LastIsochBufferLength',
        'LastIsochPacketLength0',
        'LastIsochUsbdStatus'
    )

    foreach ($name in $targetValues) {
        if ($names -contains $name) {
            $raw = $item.GetValue($name)
            if ($ntStatusNames -contains $name) {
                $formatted = ("0x{0:X8}" -f [int32]$raw)
                Write-Host ("  {0,-30} = {1}" -f $name, $formatted)
            } else {
                Write-Host ("  {0,-30} = {1}" -f $name, $raw)
            }
        } else {
            Write-Host ("  {0,-30} = not present" -f $name)
        }
    }

    $aggregateNames = @(
        'IsochUrbCountIn',
        'IsochUrbCountOut',
        'IsochNonSuccessCount',
        'IsochShortCompletionCount',
        'IsochPacketSizeMismatchCount',
        'IsochBytesRequestedIn',
        'IsochBytesCompletedIn',
        'IsochBytesRequestedOut',
        'IsochBytesCompletedOut'
    )
    $hasAggregates = ($aggregateNames | Where-Object { $names -contains $_ }).Count -eq $aggregateNames.Count

    if ($hasAggregates) {
        $urbCountIn = [uint32]([int64]$item.GetValue('IsochUrbCountIn') -band 0xFFFFFFFFL)
        $urbCountOut = [uint32]([int64]$item.GetValue('IsochUrbCountOut') -band 0xFFFFFFFFL)
        $nonSuccessCount = [uint32]([int64]$item.GetValue('IsochNonSuccessCount') -band 0xFFFFFFFFL)
        $shortCompCount = [uint32]([int64]$item.GetValue('IsochShortCompletionCount') -band 0xFFFFFFFFL)
        $pktMismatchCount = [uint32]([int64]$item.GetValue('IsochPacketSizeMismatchCount') -band 0xFFFFFFFFL)
        $bytesReqIn = [uint32]([int64]$item.GetValue('IsochBytesRequestedIn') -band 0xFFFFFFFFL)
        $bytesCompIn = [uint32]([int64]$item.GetValue('IsochBytesCompletedIn') -band 0xFFFFFFFFL)
        $bytesReqOut = [uint32]([int64]$item.GetValue('IsochBytesRequestedOut') -band 0xFFFFFFFFL)
        $bytesCompOut = [uint32]([int64]$item.GetValue('IsochBytesCompletedOut') -band 0xFFFFFFFFL)
        $totalAggUrbs = $urbCountIn + $urbCountOut

        Write-Host "`nIsochronous URB Whole-Run Aggregates:"
        Write-Host ("  {0,-30} = {1}" -f 'IsochUrbCountIn', $urbCountIn)
        Write-Host ("  {0,-30} = {1}" -f 'IsochUrbCountOut', $urbCountOut)
        Write-Host ("  {0,-30} = {1}" -f 'IsochNonSuccessCount', $nonSuccessCount)
        Write-Host ("  {0,-30} = {1}" -f 'IsochShortCompletionCount', $shortCompCount)
        Write-Host ("  {0,-30} = {1}" -f 'IsochPacketSizeMismatchCount', $pktMismatchCount)
        Write-Host ("  {0,-30} = {1}" -f 'IsochBytesRequestedIn', $bytesReqIn)
        Write-Host ("  {0,-30} = {1}" -f 'IsochBytesCompletedIn', $bytesCompIn)
        Write-Host ("  {0,-30} = {1}" -f 'IsochBytesRequestedOut', $bytesReqOut)
        Write-Host ("  {0,-30} = {1}" -f 'IsochBytesCompletedOut', $bytesCompOut)
        Write-Host ("  Requested vs Completed IN      = {0} requested, {1} completed" -f $bytesReqIn, $bytesCompIn)
        Write-Host ("  Requested vs Completed OUT     = {0} requested, {1} completed" -f $bytesReqOut, $bytesCompOut)
        Write-Host "  Note: Byte totals are 32-bit and may wrap on very long runs."
    } else {
        Write-Host "`nIsochronous URB Whole-Run Aggregates:"
        Write-Host "  Whole-run aggregates are not present (driver predates aggregate counting)."
    }

    $isochUrbCount = if ($names -contains 'IsochUrbCount') { [int64]$item.GetValue('IsochUrbCount') } else { $null }

    $sortedRecords = @()
    $hasHistory = $names -contains 'IsochUrbHistory'
    if (-not $hasHistory) {
        Write-Host "  IsochUrbHistory                = not present"
    } else {
        # ISOTEST_ISOCH_RECORD stride is 64 bytes (0x40), confirmed by AltTracking.
        # Layout (64 bytes total):
        #   0..3:   Sequence (ULONG)
        #   4:      Endpoint (UCHAR)
        #   5:      Direction (UCHAR: 0=OUT, 1=IN)
        #   6:      AltSetting (UCHAR)
        #   7:      Reserved (UCHAR)
        #   8..11:  NumberOfPackets (ULONG)
        #   12..15: TransferBufferLength (ULONG)
        #   16..19: PacketLength0 (ULONG)
        #   20..23: UsbdStatus (ULONG)
        #   24..27: BytesCompleted (ULONG)
        #   28..31: Flags (ULONG)
        #   32..35: EndpointMaxPacketSize (ULONG)
        #   36..63: Reserved2 (28 bytes)
        $stride = 64

        $blob = [byte[]]$item.GetValue('IsochUrbHistory')
        if ($null -eq $blob -or $blob.Length -eq 0) {
            Write-Host "  IsochUrbHistory                = empty (0 bytes)"
        } elseif (($blob.Length % $stride) -ne 0) {
            Write-Warn2 ("IsochUrbHistory length ({0} bytes) is not an exact multiple of stride {1}; skipping decode to avoid mis-decoding." -f $blob.Length, $stride)
        } else {
            $slotCount = [int]($blob.Length / $stride)
            $validRecords = @()
            for ($i = 0; $i -lt $slotCount; $i++) {
                $offset = $i * $stride
                $seq = [BitConverter]::ToUInt32($blob, $offset + 0)
                $ep = [int]$blob[$offset + 4]
                $dir = [int]$blob[$offset + 5]
                $alt = [int]$blob[$offset + 6]
                # offset + 7: Reserved
                $numPackets = [BitConverter]::ToUInt32($blob, $offset + 8)
                $bufLen = [BitConverter]::ToUInt32($blob, $offset + 12)
                $pktLen0 = [BitConverter]::ToUInt32($blob, $offset + 16)
                $usbd = [BitConverter]::ToUInt32($blob, $offset + 20)
                $bytesDone = [BitConverter]::ToUInt32($blob, $offset + 24)
                $flags = [BitConverter]::ToUInt32($blob, $offset + 28)
                $maxPktSize = [BitConverter]::ToUInt32($blob, $offset + 32)
                # offset + 36..63: Reserved2

                $isValid = if ($null -ne $isochUrbCount) {
                    if ($isochUrbCount -le 0) {
                        $false
                    } elseif ($isochUrbCount -lt $slotCount) {
                        $i -lt $isochUrbCount
                    } else {
                        ($numPackets -gt 0) -or ($ep -ne 0)
                    }
                } else {
                    ($numPackets -gt 0) -or ($ep -ne 0)
                }

                if ($isValid) {
                    $dirStr = if ($dir -eq 0) { 'OUT' } elseif ($dir -eq 1) { 'IN' } else { "$dir" }
                    $validRecords += [PSCustomObject]@{
                        Sequence       = $seq
                        Endpoint       = ("0x{0:X2}" -f $ep)
                        Direction      = $dirStr
                        TrackedAlt     = $alt
                        PacketCount    = $numPackets
                        BufferLength   = $bufLen
                        PacketLength0  = $pktLen0
                        UsbdStatus     = ("0x{0:X8}" -f $usbd)
                        BytesCompleted = $bytesDone
                        MaxPacketSize  = $maxPktSize
                    }
                }
            }

            $sortedRecords = @($validRecords | Sort-Object Sequence)
        }
    }

    $allStatusZero = $false
    $allOutDelivered = $false
    $outCount = 0
    $inCount = 0

    if ($sortedRecords.Count -eq 0) {
        Write-Host "  No isochronous URB records found in history."
        Write-Host "Observation: No isochronous URBs recorded in retained window."
    } else {
        $runTotalUrb = if ($hasAggregates) { $totalAggUrbs } elseif ($null -ne $isochUrbCount) { $isochUrbCount } else { $sortedRecords.Count }
        Write-Host "`nIsochronous URB History ($($sortedRecords.Count) retained of $runTotalUrb total URBs):"
        $tableStr = ($sortedRecords | Format-Table Sequence, Endpoint, Direction, TrackedAlt, PacketCount, BufferLength, PacketLength0, UsbdStatus, BytesCompleted, MaxPacketSize -AutoSize | Out-String).TrimEnd()
        Write-Host $tableStr
        Write-Host "Note: Packet geometry is derived from MaxPacketSize; TrackedAlt is diagnostic only and lags the host by one interface-setting change by design."
        Write-Host "Note: URB count does not equal CSV cell count because WinUSB splits a single OUT transfer across URBs (observed example: a 4032-byte alt-6 transfer arriving as 50 packets [3150 bytes] plus 14 packets [882 bytes])."
        Write-Host ""

        $allStatusZero = $true
        $allOutDelivered = $true

        foreach ($rec in $sortedRecords) {
            if ($rec.UsbdStatus -ne '0x00000000') {
                $allStatusZero = $false
            }
            if ($rec.Direction -eq 'OUT') {
                $outCount++
                if ($rec.BytesCompleted -ne $rec.BufferLength) {
                    $allOutDelivered = $false
                }
            } elseif ($rec.Direction -eq 'IN') {
                $inCount++
            }
        }

        Write-Host ("Observation: Retained window ({0} records: {1} OUT, {2} IN): every record UsbdStatus is zero: {3}; OUT BytesCompleted == TransferBufferLength: {4}." -f $sortedRecords.Count, $outCount, $inCount, $allStatusZero, $allOutDelivered)
    }

    if ($hasAggregates) {
        if ($totalAggUrbs -eq 0) {
            Write-Host "Verdict: No isochronous URBs recorded; device-side payload delivery cannot be evaluated."
        } else {
            $failures = @()
            if ($nonSuccessCount -gt 0) {
                $failures += ("{0} non-success completion(s)" -f $nonSuccessCount)
            }
            if ($shortCompCount -gt 0) {
                $failures += ("{0} short completion(s)" -f $shortCompCount)
            }
            if ($pktMismatchCount -gt 0) {
                $failures += ("{0} packet-size mismatch(es)" -f $pktMismatchCount)
            }
            if ($bytesCompOut -ne $bytesReqOut) {
                $failures += ("OUT bytes completed ({0}) != requested ({1})" -f $bytesCompOut, $bytesReqOut)
            }
            if ($bytesCompIn -ne $bytesReqIn) {
                $failures += ("IN bytes completed ({0}) != requested ({1})" -f $bytesCompIn, $bytesReqIn)
            }

            if ($failures.Count -eq 0) {
                Write-Host ("Verdict: Device-side confirmation SUCCESS - all {0} whole-run URB(s) verified ({1} OUT [{2} bytes], {3} IN [{4} bytes]; 0 non-success, 0 short completions, 0 packet size mismatches)." -f $totalAggUrbs, $urbCountOut, $bytesCompOut, $urbCountIn, $bytesCompIn)
            } else {
                Write-Host ("Verdict: Device-side confirmation FAILED ({0} whole-run URB(s) covered) - {1}." -f $totalAggUrbs, ($failures -join '; '))
            }
        }
    } else {
        $totalDesc = if ($null -ne $isochUrbCount) { "$isochUrbCount" } else { "unknown" }
        Write-Host "Driver predates whole-run aggregate counting; downgrading verdict to retained window only."
        if ($sortedRecords.Count -eq 0) {
            Write-Host "Verdict: No isochronous URB records found in retained window (driver predates aggregate counting); device-side payload delivery cannot be evaluated."
        } elseif ($allStatusZero -and ($outCount -gt 0) -and $allOutDelivered) {
            Write-Host ("Verdict: Retained-window confirmation SUCCESS (driver predates aggregate counting) - inspected {0} of {1} total URB(s); all {2} OUT transfer(s) in window delivered full payload." -f $sortedRecords.Count, $totalDesc, $outCount)
        } elseif ($allStatusZero -and ($outCount -eq 0)) {
            Write-Host ("Verdict: Retained-window confirmation SUCCESS (driver predates aggregate counting) - inspected {0} of {1} total URB(s); all completed with USBD_STATUS 0x00000000 (no OUT records in window)." -f $sortedRecords.Count, $totalDesc)
        } else {
            $windowReasons = @()
            if (-not $allStatusZero) {
                $windowReasons += "non-zero USBD_STATUS observed"
            }
            if ($outCount -gt 0 -and -not $allOutDelivered) {
                $windowReasons += "OUT payload under-delivery observed (BytesCompleted != TransferBufferLength)"
            }
            Write-Host ("Verdict: Retained-window confirmation FAILED (driver predates aggregate counting) - inspected {0} of {1} total URB(s); {2}." -f $sortedRecords.Count, $totalDesc, ($windowReasons -join '; '))
        }
    }
}

function Assert-IsochClockActive {
    param([int] $Expected, [int] $TimeoutSeconds = 15)
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoFlt\Parameters'
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $observed = $null
    do {
        if (Test-Path -LiteralPath $key) {
            $item = Get-Item -LiteralPath $key
            if ($item.GetValueNames() -contains 'IsochClockModeActive') {
                $observed = [int]$item.GetValue('IsochClockModeActive')
                if ($observed -eq $Expected) {
                    Write-Ok "Filter reported IsochClockModeActive = $observed; the requested mode is live."
                    return
                }
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    if ($null -eq $observed) {
        throw ("DeckBtIsoFlt never published IsochClockModeActive within $TimeoutSeconds seconds. " +
               'The filter did not load or did not attach to the child, so a measurement would say ' +
               'nothing about the bus interface. Refusing to measure.')
    }
    throw ("DeckBtIsoFlt is running mode $observed but $Expected was requested. " +
           'Refusing to measure: the result would be attributed to the wrong clock mode.')
}

function Assert-NoShippingInstrumentOverlap {
    if (-not (Test-Path -LiteralPath $shippingInf -PathType Leaf)) {
        throw "Cannot perform the overlap safety check because the generated shipping INF is missing: $shippingInf"
    }
    $shippingInfText = Get-Content -LiteralPath $shippingInf -Raw
    $shippingIds = @([regex]::Matches(
        $shippingInfText,
        '(?i)USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}') |
        ForEach-Object { $_.Value.ToUpperInvariant() } |
        Select-Object -Unique)
    if ($shippingIds.Count -ne 1) {
        throw "Expected exactly one shipping USB child identity in $shippingInf; found $($shippingIds.Count)."
    }

    $pendingPermit = $false
    $malformedPermit = $false
    $key = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters'
    if (Test-Path -LiteralPath $key) {
        $item = Get-Item -LiteralPath $key
        if ($item.GetValueNames() -contains 'Enabled') {
            $kind = $item.GetValueKind('Enabled')
            $value = $item.GetValue('Enabled', $null, 'DoNotExpandEnvironmentNames')
            $pendingPermit = ($kind -eq [Microsoft.Win32.RegistryValueKind]::DWord -and
                              [uint32]$value -eq 1)
            $malformedPermit = -not (
                $kind -eq [Microsoft.Win32.RegistryValueKind]::DWord -and
                ([uint32]$value -eq 0 -or [uint32]$value -eq 1))
        }
    }

    # Enabled is a one-shot permit and may already be consumed (zero) while the child remains.
    # Presence is therefore checked independently; no persistent Active registry state is assumed.
    $shippingPattern = "$($shippingIds[0])\*"
    $shippingChildren = @(Get-PnpDevice -PresentOnly -ErrorAction Stop |
        Where-Object { $_.InstanceId -like $shippingPattern })
    if ($malformedPermit) {
        Write-Warn2 'Enabled exists but is not REG_DWORD 0/1; it is not a valid one-shot arm permit.'
    }
    if ($pendingPermit -or $shippingChildren.Count -gt 0) {
        $reason = if ($pendingPermit) {
            'a pending Enabled REG_DWORD 1 permit'
        } else {
            'a present shipping USB child'
        }
        throw ('SAFETY REFUSAL: detected ' + $reason + '. Run tools\M1-DISARM.cmd first. ' +
               'The vendor-class isotest child is not a Bluetooth adapter and does not consume ' +
               'an adapter slot; this refusal prevents concurrent operation of the shipping ' +
               'instrument and the throwaway measurement instrument.')
    }
    Write-Ok "No pending shipping permit or present $($shippingIds[0]) child detected."
}

function Wait-Instrument {
    param([bool] $Present, [int] $TimeoutSeconds = 15)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $isPresent = $null -ne (Get-InstrumentDevice)
        if ($isPresent -eq $Present) { return }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    $wanted = if ($Present) { 'appear' } else { 'be removed' }
    throw "Timed out after $TimeoutSeconds seconds waiting for $instanceId to $wanted."
}

function Remove-OwnedInstrument {
    if ($DryRun) {
        Write-Host "[DRY-RUN] pnputil.exe /remove-device `"$instanceId`" /subtree"
        return
    }
    if ($null -eq (Get-InstrumentDevice)) {
        Write-Ok "$instanceId is already absent."
        return
    }
    [void](Invoke-CheckedNative pnputil.exe @('/remove-device', $instanceId, '/subtree'))
    Wait-Instrument -Present $false
    Write-Ok "Removed exact instrument devnode $instanceId (including only its subtree)."
}


function Invoke-Rollback {
    Write-Step "Rollback removes only the exact instrument devnode $instanceId."
    Remove-OwnedInstrument
    Write-Ok 'Staged DriverStore packages are intentionally retained: without a run-owned inventory,'
    Write-Ok 'deleting a preexisting isotest package would violate conservative ownership.'
}

try {
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $logPath) -Force)
    Start-Transcript -Path $logPath -ErrorAction Stop | Out-Null
    $transcriptStarted = $true
    Write-Host 'DeckBtIsoTest Stage 2 operator'
    Write-Host "  Package:    $package"
    Write-Host "  IsochClock: $IsochClock"
    Write-Host "  Harness:    $harness"
    Write-Host "  Report:     $ReportPath"
    Write-Host "  Log:        $logPath"
    Write-Host "  Reverse:    tools\M2-ISO-MEASURE.cmd -Rollback"

    Show-RunIdentity
    Assert-Elevated
    if ($Rollback) {
        Invoke-Rollback
    } else {
        Assert-PackageContract
        Assert-NoShippingInstrumentOverlap

        if ($DryRun) {
            Write-Step 'DryRun: no UAC, package staging, devnode creation, registry/device mutation, or cleanup mutation.'
            $isochClockVal = if ($IsochClock -eq 'Synthesize') { 2 } else { 1 }
            Write-Host "[DRY-RUN] Would write HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoFlt\Parameters\IsochClockMode = $isochClockVal ($IsochClock)"
            Write-Step "DryRun writes only the report ($ReportPath) and diagnostic log ($logPath)."
            $harnessOutput = [System.Collections.Generic.List[string]]::new()
            [void](Invoke-CheckedNative $harness @('--plan', '--csv', $ReportPath) @(0) $harnessOutput)
            $script:harnessBuildStamp = Confirm-HarnessBuildStamp $harnessOutput
        } else {
            if ($null -ne (Get-InstrumentDevice)) {
                throw "Conservative refusal: $instanceId existed before this run. Run the explicit rollback only if you intend to remove it."
            }
            if (-not (Test-Path -LiteralPath $devGen -PathType Leaf)) {
                throw "Required EWDK devgen.exe is missing: $devGen"
            }

            # Seed IsochClockMode for DeckBtIsoFlt before devnode creation
            $filterParamsKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoFlt\Parameters'
            $isochClockVal = if ($IsochClock -eq 'Synthesize') { 2 } else { 1 }
            if (-not (Test-Path -LiteralPath $filterParamsKey)) {
                [void](New-Item -Path $filterParamsKey -Force)
            }
            Set-ItemProperty -LiteralPath $filterParamsKey -Name 'IsochClockMode' -Value $isochClockVal -Type DWord -Force
            Write-Ok "Configured $filterParamsKey\IsochClockMode = $isochClockVal ($IsochClock)."

            # Stage the current package before publishing the root. This prevents an older staged
            # isotest package from binding in the gap between devgen and /add-driver /install.
            [void](Invoke-CheckedNative pnputil.exe @('/add-driver', $inf) @(0, 259, 3010))
            [void](Invoke-CheckedNative $devGen @('/add', '/bus', 'ROOT', '/instanceid', 'DECKBTISOTEST', '/hardwareid', $hardwareId))
            $createdByRun = $true
            [void](Invoke-CheckedNative pnputil.exe @('/add-driver', $inf, '/install') @(0, 259, 3010))
            [void](Invoke-CheckedNative pnputil.exe @('/scan-devices') @(0, 259, 3010))
            Wait-Instrument -Present $true
            Assert-IsochClockActive -Expected $isochClockVal
            $harnessOutput = [System.Collections.Generic.List[string]]::new()
            [void](Invoke-CheckedNative $harness @('--csv', $ReportPath) @(0) $harnessOutput)
            $script:harnessBuildStamp = Confirm-HarnessBuildStamp $harnessOutput
            Write-Ok "Measurement completed: $ReportPath"
            Show-FilterDiagnostics
            Show-DriverDiagnostics
        }
    }
} catch {
    Write-Host "[x] $($_.Exception.Message)" -ForegroundColor Red
    $primaryExit = 1
    if (-not $DryRun -and -not $Rollback -and $createdByRun) {
        try {
            Show-FilterDiagnostics
        } catch {
            Write-Warn2 "Could not read filter diagnostics: $($_.Exception.Message)"
        }
        try {
            Show-DriverDiagnostics
        } catch {
            Write-Warn2 "Could not read driver diagnostics: $($_.Exception.Message)"
        }
    }
} finally {
    if (-not $Rollback -and $createdByRun) {
        try {
            Remove-OwnedInstrument
        } catch {
            Write-Host "[x] Cleanup failed: $($_.Exception.Message)" -ForegroundColor Red
            if ($primaryExit -eq 0) { $primaryExit = 1 }
        }
    }
    Write-Host "Run exit code: $primaryExit"
    if (-not $DryRun -and -not $Rollback) {
        Write-Host ""
        Write-Host "THIS RUN"
        Write-Host "  Mode:          $IsochClock"
        Write-Host "  Report:        $fullReportPath"
        Write-Host "  Harness build: $harnessBuildStamp"
    }
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
}

exit $primaryExit
