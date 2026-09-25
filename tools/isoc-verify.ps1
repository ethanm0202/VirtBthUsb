<#
.SYNOPSIS
    Isochronous Regression Verifier.
    Asserts whether an isochronous measurement CSV matches the isochronous reference contract.

.DESCRIPTION
    Given a measurement CSV report from tools/isotest-run.ps1 (-IsochClock Synthesize or Observe),
    validates that the measurement holds the isochronous reference expectations:
      - CSV header matches expected contract exactly
      - Total data cell count (432)
      - Accepted totals overall (216) and per direction (54 IN, 162 OUT)
      - Zero exact-category cells rejected
      - Rejected-by-category counts (zero: 108, sub: 54, oversize: 54)
      - Accepted IN row byte arithmetic (BytesTransferred == INPacketCountArgument * NominalBytesPerInterval)
      - Accepted IN total byte count (30,772)
      - Deliberate negative controls fail with exact documented stages and Win32 errors (GEOMETRY.md)
      - Regression check: zero negative controls erroneously accepted
      - Accepted OUT rows carry NOT_REPORTED in UsbdStatus and BytesTransferred
      - Kernel aggregate cross-check (IsochBytesCompletedIn, IsochNonSuccessCount, etc.) when registry key exists

.PARAMETER Report
    Path to the measurement CSV report to verify. Required.

.PARAMETER Expectations
    Path to EXPECTATIONS.json. Defaults to reference/isochronous/EXPECTATIONS.json.

.PARAMETER DriverParametersKey
    Registry path to DeckBtIsoTest driver parameters. Defaults to HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoTest\Parameters.
    If the key is absent or unreadable, the kernel cross-check is reported as skipped.

.OUTPUTS
    Exit 0: All performed checks passed (intact reference).
    Exit 1: One or more checks failed (regression or drift detected).
    Exit 2: Report or expectations file missing or unreadable.
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Report,

    [Parameter(Position = 1)]
    [string]$Expectations,

    [Parameter()]
    [string]$DriverParametersKey = "HKLM:\SYSTEM\CurrentControlSet\Services\DeckBtIsoTest\Parameters"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Exit code tracking
$failedChecks = 0

# Helper: Parse hex or decimal integer string safely
function Parse-ErrorCode([string]$val) {
    if ([string]::IsNullOrWhiteSpace($val)) { return $null }
    $t = $val.Trim()
    try {
        if ($t.StartsWith("0x", [System.StringComparison]::OrdinalIgnoreCase)) {
            return [Convert]::ToUInt32($t.Substring(2), 16)
        } else {
            return [Convert]::ToUInt32($t, 10)
        }
    } catch {
        return $null
    }
}
if ([string]::IsNullOrWhiteSpace($Expectations)) {
    if ($PSScriptRoot) {
        $candidate = Join-Path $PSScriptRoot "../reference/isochronous/EXPECTATIONS.json"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $Expectations = (Resolve-Path -LiteralPath $candidate).Path
        } else {
            $Expectations = $candidate
        }
    } else {
        $Expectations = "reference/isochronous/EXPECTATIONS.json"
    }
}


# -----------------------------------------------------------------------------
# Input Validation (Exit 2 if missing or unreadable)
# -----------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Report)) {
    Write-Host "[-] FAIL: -Report parameter is required." -ForegroundColor Red
    exit 2
}

if (-not (Test-Path -LiteralPath $Report -PathType Leaf)) {
    Write-Host "[-] FAIL: Report file missing or unreadable: '$Report'" -ForegroundColor Red
    exit 2
}

try {
    $reportContent = Get-Content -LiteralPath $Report -ErrorAction Stop
} catch {
    Write-Host "[-] FAIL: Report file cannot be read: $_" -ForegroundColor Red
    exit 2
}

if ([string]::IsNullOrWhiteSpace($Expectations)) {
    Write-Host "[-] FAIL: -Expectations parameter is required." -ForegroundColor Red
    exit 2
}

if (-not (Test-Path -LiteralPath $Expectations -PathType Leaf)) {
    Write-Host "[-] FAIL: Expectations file missing or unreadable: '$Expectations'" -ForegroundColor Red
    exit 2
}

try {
    $expRaw = Get-Content -LiteralPath $Expectations -Raw -ErrorAction Stop
    $expJson = $expRaw | ConvertFrom-Json -ErrorAction Stop
} catch {
    Write-Host "[-] FAIL: Expectations file cannot be read or parsed: $_" -ForegroundColor Red
    exit 2
}

Write-Host "========================================================================"
Write-Host " Isochronous Regression Verifier"
Write-Host " Report:       $Report"
Write-Host " Expectations: $Expectations"
Write-Host "========================================================================"

# -----------------------------------------------------------------------------
# 1. Parse CSV and verify Header
# -----------------------------------------------------------------------------
$nonEmptyLines = @($reportContent | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
if ($nonEmptyLines.Count -eq 0) {
    Write-Host "[-] FAIL: Report file is empty" -ForegroundColor Red
    exit 1
}

$headerLine = $nonEmptyLines[0]
$dataLines = @($nonEmptyLines | Select-Object -Skip 1 | Where-Object { -not $_.StartsWith("#") })

$expectedHeader = $expJson.csvHeader
if ($headerLine -eq $expectedHeader) {
    Write-Host "[PASS] Header string: matches expected contract exactly"
} else {
    Write-Host "[-] FAIL: Header string mismatch:"
    Write-Host "         Observed: '$headerLine'"
    Write-Host "         Expected: '$expectedHeader'"
    $failedChecks++
}

try {
    $csvRows = @($headerLine; $dataLines) | ConvertFrom-Csv -ErrorAction Stop
} catch {
    Write-Host "[-] FAIL: Failed to parse CSV rows: $_" -ForegroundColor Red
    exit 1
}

# -----------------------------------------------------------------------------
# 2. Row Counts and Category Aggregates
# -----------------------------------------------------------------------------
$totalDataRows = $csvRows.Count
$expectedTotalRows = [int]$expJson.totalCells
if ($totalDataRows -eq $expectedTotalRows) {
    Write-Host "[PASS] Total data rows: observed $totalDataRows, expected $expectedTotalRows"
} else {
    Write-Host "[-] FAIL: Total data rows: observed $totalDataRows, expected $expectedTotalRows"
    $failedChecks++
}

$acceptedRows = @($csvRows | Where-Object { $_.Result -eq 'ACCEPTED' })
$acceptedCount = $acceptedRows.Count
$expectedAcceptedCount = [int]$expJson.acceptedTotal
if ($acceptedCount -eq $expectedAcceptedCount) {
    Write-Host "[PASS] Total accepted cells: observed $acceptedCount, expected $expectedAcceptedCount"
} else {
    Write-Host "[-] FAIL: Total accepted cells: observed $acceptedCount, expected $expectedAcceptedCount"
    $failedChecks++
}

$acceptedInRows = @($acceptedRows | Where-Object { $_.Direction -eq 'IN' })
$acceptedInCount = $acceptedInRows.Count
$expectedAcceptedIn = [int]$expJson.acceptedIn
if ($acceptedInCount -eq $expectedAcceptedIn) {
    Write-Host "[PASS] Accepted IN cells: observed $acceptedInCount, expected $expectedAcceptedIn"
} else {
    Write-Host "[-] FAIL: Accepted IN cells: observed $acceptedInCount, expected $expectedAcceptedIn"
    $failedChecks++
}

$acceptedOutRows = @($acceptedRows | Where-Object { $_.Direction -eq 'OUT' })
$acceptedOutCount = $acceptedOutRows.Count
$expectedAcceptedOut = [int]$expJson.acceptedOut
if ($acceptedOutCount -eq $expectedAcceptedOut) {
    Write-Host "[PASS] Accepted OUT cells: observed $acceptedOutCount, expected $expectedAcceptedOut"
} else {
    Write-Host "[-] FAIL: Accepted OUT cells: observed $acceptedOutCount, expected $expectedAcceptedOut"
    $failedChecks++
}

$exactRejectedRows = @($csvRows | Where-Object { $_.Category -eq 'exact' -and $_.Result -eq 'REJECTED' })
$exactRejectedCount = $exactRejectedRows.Count
$expectedExactRejected = [int]$expJson.exactRejected
if ($exactRejectedCount -eq $expectedExactRejected) {
    Write-Host "[PASS] Exact category rejected cells: observed $exactRejectedCount, expected $expectedExactRejected"
} else {
    Write-Host "[-] FAIL: Exact category rejected cells: observed $exactRejectedCount, expected $expectedExactRejected"
    $failedChecks++
}

$rejectedRows = @($csvRows | Where-Object { $_.Result -eq 'REJECTED' })

$rejectedZeroCount = @($rejectedRows | Where-Object { $_.Category -eq 'zero' }).Count
$expectedRejectedZero = [int]$expJson.rejectedByCategory.zero
if ($rejectedZeroCount -eq $expectedRejectedZero) {
    Write-Host "[PASS] Rejected zero cells: observed $rejectedZeroCount, expected $expectedRejectedZero"
} else {
    Write-Host "[-] FAIL: Rejected zero cells: observed $rejectedZeroCount, expected $expectedRejectedZero"
    $failedChecks++
}

$rejectedSubCount = @($rejectedRows | Where-Object { $_.Category -eq 'sub' }).Count
$expectedRejectedSub = [int]$expJson.rejectedByCategory.sub
if ($rejectedSubCount -eq $expectedRejectedSub) {
    Write-Host "[PASS] Rejected sub cells: observed $rejectedSubCount, expected $expectedRejectedSub"
} else {
    Write-Host "[-] FAIL: Rejected sub cells: observed $rejectedSubCount, expected $expectedRejectedSub"
    $failedChecks++
}

$rejectedOversizeCount = @($rejectedRows | Where-Object { $_.Category -eq 'oversize' }).Count
$expectedRejectedOversize = [int]$expJson.rejectedByCategory.oversize
if ($rejectedOversizeCount -eq $expectedRejectedOversize) {
    Write-Host "[PASS] Rejected oversize cells: observed $rejectedOversizeCount, expected $expectedRejectedOversize"
} else {
    Write-Host "[-] FAIL: Rejected oversize cells: observed $rejectedOversizeCount, expected $expectedRejectedOversize"
    $failedChecks++
}

# -----------------------------------------------------------------------------
# 3. Accepted IN Row Arithmetic
# -----------------------------------------------------------------------------
$inByteErrors = 0
$totalAcceptedInBytes = 0
foreach ($row in $acceptedInRows) {
    $pktCount = 0
    $nominal = 0
    $transferred = 0

    $parsedPkt = [int64]::TryParse($row.INPacketCountArgument, [ref]$pktCount)
    $parsedNom = [int64]::TryParse($row.NominalBytesPerInterval, [ref]$nominal)
    $parsedTx = [int64]::TryParse($row.BytesTransferred, [ref]$transferred)

    if ($parsedPkt -and $parsedNom -and $parsedTx) {
        $expectedBytes = $pktCount * $nominal
        if ($transferred -ne $expectedBytes) {
            $inByteErrors++
        }
        $totalAcceptedInBytes += $transferred
    } else {
        $inByteErrors++
    }
}

if ($inByteErrors -eq 0 -and $acceptedInCount -gt 0) {
    Write-Host "[PASS] Accepted IN byte arithmetic: all $acceptedInCount rows satisfy BytesTransferred == INPacketCountArgument * NominalBytesPerInterval"
} elseif ($acceptedInCount -eq 0) {
    Write-Host "[-] FAIL: Accepted IN byte arithmetic: 0 accepted IN rows found to verify"
    $failedChecks++
} else {
    Write-Host "[-] FAIL: Accepted IN byte arithmetic: $inByteErrors of $acceptedInCount rows mismatched"
    $failedChecks++
}

$expectedInBytesTotal = [int64]$expJson.inBytesTotal
if ($totalAcceptedInBytes -eq $expectedInBytesTotal) {
    Write-Host "[PASS] Accepted IN total bytes: observed $totalAcceptedInBytes, expected $expectedInBytesTotal"
} else {
    Write-Host "[-] FAIL: Accepted IN total bytes: observed $totalAcceptedInBytes, expected $expectedInBytesTotal"
    $failedChecks++
}

# -----------------------------------------------------------------------------
# 4. Negative Controls Contract (reference/isochronous/GEOMETRY.md)
#
# Deliberate negative controls must produce exact Win32 errors at exact stages:
#   zero OUT:    54 cells -> REJECTED at SUBMIT_TRANSFER with 0x000006F8 (ERROR_INVALID_USER_BUFFER)
#   zero IN:     54 cells -> REJECTED at SUBMIT_TRANSFER with 0x00000057 (ERROR_INVALID_PARAMETER)
#   sub IN:      54 cells -> REJECTED at COMPLETION      with 0x0000007A (ERROR_INSUFFICIENT_BUFFER)
#   oversize IN: 54 cells -> REJECTED at SUBMIT_TRANSFER with 0x000006F8 (ERROR_INVALID_USER_BUFFER)
# -----------------------------------------------------------------------------

# Regression check: Any negative control row that was ACCEPTED is a severe regression.
$regressedNegativeControls = @($csvRows | Where-Object {
    ($_.Result -eq 'ACCEPTED') -and (
        ($_.Direction -eq 'OUT' -and $_.Category -eq 'zero') -or
        ($_.Direction -eq 'IN' -and $_.Category -in @('zero', 'sub', 'oversize'))
    )
})

if ($regressedNegativeControls.Count -eq 0) {
    Write-Host "[PASS] Negative controls regression check: 0 of 216 negative controls were accepted"
} else {
    Write-Host "[-] FAIL: REGRESSION DETECTED: $($regressedNegativeControls.Count) negative control cell(s) were ACCEPTED (expected 0):"
    foreach ($r in $regressedNegativeControls) {
        Write-Host "         Alt $($r.Alt), Dir $($r.Direction), Cat $($r.Category), Packets $($r.INPacketCountArgument): Result=ACCEPTED"
    }
    $failedChecks++
}

$negativeControlSpecs = @(
    @{
        Label = "zero OUT"
        Direction = "OUT"
        Category = "zero"
        ExpectedCount = 54
        ExpectedFailedAt = "SUBMIT_TRANSFER"
        ExpectedWin32Code = 0x6F8
        ExpectedWin32Str = "0x000006F8 (1784: ERROR_INVALID_USER_BUFFER)"
    },
    @{
        Label = "zero IN"
        Direction = "IN"
        Category = "zero"
        ExpectedCount = 54
        ExpectedFailedAt = "SUBMIT_TRANSFER"
        ExpectedWin32Code = 0x57
        ExpectedWin32Str = "0x00000057 (87: ERROR_INVALID_PARAMETER)"
    },
    @{
        Label = "sub IN"
        Direction = "IN"
        Category = "sub"
        ExpectedCount = 54
        ExpectedFailedAt = "COMPLETION"
        ExpectedWin32Code = 0x7A
        ExpectedWin32Str = "0x0000007A (122: ERROR_INSUFFICIENT_BUFFER)"
    },
    @{
        Label = "oversize IN"
        Direction = "IN"
        Category = "oversize"
        ExpectedCount = 54
        ExpectedFailedAt = "SUBMIT_TRANSFER"
        ExpectedWin32Code = 0x6F8
        ExpectedWin32Str = "0x000006F8 (1784: ERROR_INVALID_USER_BUFFER)"
    }
)

foreach ($spec in $negativeControlSpecs) {
    $rows = @($csvRows | Where-Object { $_.Direction -eq $spec.Direction -and $_.Category -eq $spec.Category })
    if ($rows.Count -ne $spec.ExpectedCount) {
        Write-Host "[-] FAIL: Negative control $($spec.Label): observed $($rows.Count) rows, expected $($spec.ExpectedCount)"
        $failedChecks++
        continue
    }

    $failedAtMismatches = 0
    $errorCodeMismatches = 0
    $notRejected = 0

    foreach ($r in $rows) {
        if ($r.Result -ne 'REJECTED') {
            $notRejected++
        }
        if ($r.FailedAt -ne $spec.ExpectedFailedAt) {
            $failedAtMismatches++
        }
        $code = Parse-ErrorCode $r.Win32Error
        if ($null -eq $code -or $code -ne $spec.ExpectedWin32Code) {
            $errorCodeMismatches++
        }
    }

    if ($notRejected -eq 0 -and $failedAtMismatches -eq 0 -and $errorCodeMismatches -eq 0) {
        Write-Host "[PASS] Negative control $($spec.Label): all $($rows.Count) rejected at $($spec.ExpectedFailedAt) with $($spec.ExpectedWin32Str)"
    } else {
        Write-Host "[-] FAIL: Negative control $($spec.Label): contract violation in $($rows.Count) rows (not rejected: $notRejected, wrong FailedAt: $failedAtMismatches, wrong Win32Error: $errorCodeMismatches)"
        $failedChecks++
    }
}

# -----------------------------------------------------------------------------
# 5. Accepted OUT Rows NOT_REPORTED Contract
# -----------------------------------------------------------------------------
if ($acceptedOutCount -gt 0) {
    $outNotReportedErrors = 0
    foreach ($r in $acceptedOutRows) {
        if ($r.UsbdStatus -ne 'NOT_REPORTED' -or $r.BytesTransferred -ne 'NOT_REPORTED') {
            $outNotReportedErrors++
        }
    }
    if ($outNotReportedErrors -eq 0) {
        Write-Host "[PASS] Accepted OUT status: all $acceptedOutCount rows carry UsbdStatus=NOT_REPORTED and BytesTransferred=NOT_REPORTED"
    } else {
        Write-Host "[-] FAIL: Accepted OUT status: $outNotReportedErrors of $acceptedOutCount rows did not carry NOT_REPORTED"
        $failedChecks++
    }
} else {
    Write-Host "[-] FAIL: Accepted OUT status: 0 accepted OUT rows found to verify"
    $failedChecks++
}

# -----------------------------------------------------------------------------
# 6. Kernel Aggregate Cross-Check (Registry)
# -----------------------------------------------------------------------------
$keyExists = $false
try {
    $keyExists = Test-Path -LiteralPath $DriverParametersKey -ErrorAction SilentlyContinue
} catch {
    $keyExists = $false
}

if ($keyExists) {
    try {
        $regProps = Get-ItemProperty -LiteralPath $DriverParametersKey -ErrorAction Stop

        # Cross-check 1: IsochBytesCompletedIn
        $kBytesCompletedIn = $regProps.IsochBytesCompletedIn
        if ($null -ne $kBytesCompletedIn) {
            $kBytesCompletedInVal = [int64]$kBytesCompletedIn
            if ($totalAcceptedInBytes -eq $kBytesCompletedInVal) {
                Write-Host "[PASS] Kernel aggregate IsochBytesCompletedIn: observed $kBytesCompletedInVal matches CSV accepted IN total ($totalAcceptedInBytes)"
            } else {
                Write-Host "[-] FAIL: Kernel aggregate IsochBytesCompletedIn: registry has $kBytesCompletedInVal, CSV accepted IN total is $totalAcceptedInBytes"
                $failedChecks++
            }
        } else {
            Write-Host "[-] FAIL: Kernel aggregate IsochBytesCompletedIn property not found in $DriverParametersKey"
            $failedChecks++
        }

        # Cross-check 2: IsochNonSuccessCount == 0
        $kNonSuccess = $regProps.IsochNonSuccessCount
        if ($null -ne $kNonSuccess) {
            $kNonSuccessVal = [int64]$kNonSuccess
            if ($kNonSuccessVal -eq 0) {
                Write-Host "[PASS] Kernel aggregate IsochNonSuccessCount: observed $kNonSuccessVal, expected 0"
            } else {
                Write-Host "[-] FAIL: Kernel aggregate IsochNonSuccessCount: observed $kNonSuccessVal, expected 0"
                $failedChecks++
            }
        } else {
            Write-Host "[-] FAIL: Kernel aggregate IsochNonSuccessCount property not found in $DriverParametersKey"
            $failedChecks++
        }

        # Cross-check 3: IsochShortCompletionCount == 0
        $kShortComp = $regProps.IsochShortCompletionCount
        if ($null -ne $kShortComp) {
            $kShortCompVal = [int64]$kShortComp
            if ($kShortCompVal -eq 0) {
                Write-Host "[PASS] Kernel aggregate IsochShortCompletionCount: observed $kShortCompVal, expected 0"
            } else {
                Write-Host "[-] FAIL: Kernel aggregate IsochShortCompletionCount: observed $kShortCompVal, expected 0"
                $failedChecks++
            }
        } else {
            Write-Host "[-] FAIL: Kernel aggregate IsochShortCompletionCount property not found in $DriverParametersKey"
            $failedChecks++
        }

        # Cross-check 4: IsochPacketSizeMismatchCount == 0
        $kMismatch = $regProps.IsochPacketSizeMismatchCount
        if ($null -ne $kMismatch) {
            $kMismatchVal = [int64]$kMismatch
            if ($kMismatchVal -eq 0) {
                Write-Host "[PASS] Kernel aggregate IsochPacketSizeMismatchCount: observed $kMismatchVal, expected 0"
            } else {
                Write-Host "[-] FAIL: Kernel aggregate IsochPacketSizeMismatchCount: observed $kMismatchVal, expected 0"
                $failedChecks++
            }
        } else {
            Write-Host "[-] FAIL: Kernel aggregate IsochPacketSizeMismatchCount property not found in $DriverParametersKey"
            $failedChecks++
        }
    } catch {
        Write-Host "[SKIP] Kernel aggregate cross-check: key '$DriverParametersKey' could not be read: $_"
    }
} else {
    Write-Host "[SKIP] Kernel aggregate cross-check: registry key '$DriverParametersKey' is absent"
}

# -----------------------------------------------------------------------------
# Final Verdict
# -----------------------------------------------------------------------------
Write-Host ""
if ($failedChecks -eq 0) {
    Write-Host "========================================================================" -ForegroundColor Green
    Write-Host " ISOCHRONOUS REGRESSION VERDICT: PASS (All checks passed)" -ForegroundColor Green
    Write-Host "========================================================================" -ForegroundColor Green
    exit 0
} else {
    Write-Host "========================================================================" -ForegroundColor Red
    Write-Host " ISOCHRONOUS REGRESSION VERDICT: FAIL ($failedChecks check(s) failed)" -ForegroundColor Red
    Write-Host "========================================================================" -ForegroundColor Red
    exit 1
}
