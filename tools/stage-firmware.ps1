<#
    tools\stage-firmware.ps1 - Stage Qualcomm Bluetooth firmware files for DeckBtUsb.

    Copies hpbtfw21.tlv, hpnv21.bin, hpnv21g.bin, hpnv21.309, and hpnv21g.309 from the
    machine's local DriverStore FileRepository into the driver package source directory
    so that the INF can stage them to %12%\DeckBtUsb (\SystemRoot\System32\drivers\DeckBtUsb\).

    Exit codes:
      0 = success (all files copied and SHA-256 verified)
      1 = failure or refusal to proceed (hash mismatch, size mismatch, copy error)
      2 = precondition missing (vendor DriverStore repo or source firmware file not found)
#>

[CmdletBinding()]
param(
    [string]$Destination,
    [string]$Configuration
)

$ErrorActionPreference = 'Stop'

$script:Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$script:DefaultDest = Join-Path $script:Root 'src\driver'

function Write-Step { param($m) Write-Host "[*] $m" -ForegroundColor Cyan }
function Write-Ok   { param($m) Write-Host "[+] $m" -ForegroundColor Green }
function Write-Warn { param($m) Write-Host "[!] $m" -ForegroundColor Yellow }
function Write-Err  { param($m) Write-Host "[-] $m" -ForegroundColor Red }

Write-Step "DeckBtUsb - Qualcomm Firmware Staging"

# 1. Locate vendor DriverStore directory
$fileRepo = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'
if (-not (Test-Path -LiteralPath $fileRepo)) {
    Write-Err "PRECONDITION FAILED: DriverStore FileRepository directory not found at $fileRepo"
    exit 2
}

Write-Step "Searching FileRepository for qcbtuart.inf_amd64_* directories..."
$candidates = @(Get-ChildItem -LiteralPath $fileRepo -Directory -Filter 'qcbtuart.inf_amd64_*' |
    Sort-Object LastWriteTime -Descending)

if ($candidates.Count -eq 0) {
    Write-Err "PRECONDITION FAILED: No qcbtuart.inf_amd64_* directory found in $fileRepo"
    exit 2
}

$selectedRepo = $null
if ($candidates.Count -eq 1) {
    $selectedRepo = $candidates[0]
    Write-Ok "Found 1 vendor DriverStore directory:"
    Write-Host "    Path:          $($selectedRepo.FullName)"
    Write-Host "    LastWriteTime: $($selectedRepo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"
} else {
    Write-Step "Found $($candidates.Count) candidate directories matching qcbtuart.inf_amd64_*:"
    foreach ($c in $candidates) {
        Write-Host "    - $($c.Name) (LastWriteTime: $($c.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))"
    }
    $selectedRepo = $candidates[0]
    Write-Ok "Selected newest directory by LastWriteTime: $($selectedRepo.FullName)"
    Write-Host "    Reason: newest directory write time ($($selectedRepo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))"
}

# 2. Verify required source firmware files exist
$firmwareFiles = @('hpbtfw21.tlv', 'hpnv21.bin', 'hpnv21g.bin', 'hpnv21.309', 'hpnv21g.309')
foreach ($fn in $firmwareFiles) {
    $srcFile = Join-Path $selectedRepo.FullName $fn
    if (-not (Test-Path -LiteralPath $srcFile)) {
        Write-Err "PRECONDITION FAILED: Required source firmware file missing from DriverStore:"
        Write-Host "    File: $srcFile"
        exit 2
    }
}

# 3. Determine target destination directories
$destDirs = [System.Collections.Generic.List[string]]::new()

if ($Destination) {
    $destDirs.Add($Destination)
} else {
    $destDirs.Add($script:DefaultDest)
    if ($Configuration) {
        $pkgDir = Join-Path $script:Root "src\driver\x64\$Configuration\deckbtusb"
        if (Test-Path -LiteralPath $pkgDir) {
            $destDirs.Add($pkgDir)
        }
    }
}

# 4. Copy and verify each firmware file
foreach ($targetDir in $destDirs) {
    if (-not (Test-Path -LiteralPath $targetDir)) {
        Write-Step "Creating target directory: $targetDir"
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    }

    Write-Step "Staging firmware into: $targetDir"
    foreach ($fn in $firmwareFiles) {
        $srcPath  = Join-Path $selectedRepo.FullName $fn
        $destPath = Join-Path $targetDir $fn

        $srcItem = Get-Item -LiteralPath $srcPath
        $srcSize = $srcItem.Length
        $srcHash = (Get-FileHash -LiteralPath $srcPath -Algorithm SHA256).Hash.ToLowerInvariant()

        Write-Host "    Copying $fn ($srcSize bytes)..."
        try {
            Copy-Item -LiteralPath $srcPath -Destination $destPath -Force
        } catch {
            Write-Err "FAILURE: Failed to copy $srcPath to $destPath : $_"
            exit 1
        }

        if (-not (Test-Path -LiteralPath $destPath)) {
            Write-Err "FAILURE: Destination file does not exist after copy: $destPath"
            exit 1
        }

        $destItem = Get-Item -LiteralPath $destPath
        $destSize = $destItem.Length
        $destHash = (Get-FileHash -LiteralPath $destPath -Algorithm SHA256).Hash.ToLowerInvariant()

        if ($destSize -ne $srcSize) {
            Write-Err "FAILURE: Truncated copy for $fn!"
            Write-Host "    Source size:      $srcSize bytes"
            Write-Host "    Destination size: $destSize bytes"
            exit 1
        }

        if ($destHash -ne $srcHash) {
            Write-Err "FAILURE: SHA-256 hash mismatch for $fn!"
            Write-Host "    Source SHA-256:      $srcHash"
            Write-Host "    Destination SHA-256: $destHash"
            exit 1
        }

        Write-Ok "Verified $fn"
        Write-Host "       Size:    $destSize bytes"
        Write-Host "       SHA-256: $destHash"
        Write-Host "       Target:  $destPath"
    }
}

Write-Ok "Firmware staging complete and verified."
exit 0
