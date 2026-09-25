# check-isoc-reference.ps1 - assert that the isochronous reference evidence has not drifted or been modified.
#
# reference\isochronous\ is the project's regression target for isochronous transfer.
# This script recomputes SHA-256 for every file listed in MANIFEST.sha256 and verifies:
#   1. Every file in MANIFEST.sha256 exists and matches its recorded hash exactly.
#   2. No files listed in MANIFEST.sha256 are missing.
#   3. No extraneous / untracked files are present in reference\isochronous\ (excluding MANIFEST.sha256).
#
# Exit 0 = reference intact. Exit 1 = mismatch, missing file, or untracked file.
# No elevation, read-only.

[CmdletBinding()]
param(
    [string]$ReferenceDir = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ReferenceDir)) {
    $ReferenceDir = Join-Path $PSScriptRoot "..\reference\isochronous"
}

$refPath = [System.IO.Path]::GetFullPath($ReferenceDir)
$manifestPath = Join-Path $refPath "MANIFEST.sha256"

if (-not (Test-Path -LiteralPath $refPath -PathType Container)) {
    Write-Host "[-] FAIL: no reference tree at reference\isochronous"
    exit 1
}

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    Write-Host "[-] FAIL: no committed manifest at reference\isochronous\MANIFEST.sha256"
    exit 1
}

$manifestLines = Get-Content -LiteralPath $manifestPath
if ($null -eq $manifestLines -or $manifestLines.Count -eq 0) {
    Write-Host "[-] FAIL: MANIFEST.sha256 is empty"
    exit 1
}

$issues = 0
$checked = 0
$manifestFiles = @{}

foreach ($rawLine in $manifestLines) {
    $line = $rawLine.Trim()
    if ([string]::IsNullOrWhiteSpace($line) -or $line.StartsWith("#")) {
        continue
    }

    # Format: <sha256>  <relative/path> (two spaces or whitespace separated)
    if ($line -match '^([0-9a-fA-F]{64})\s+(.+)$') {
        $expectedHash = $matches[1].ToLowerInvariant()
        $relPath = $matches[2].Trim()
        
        # Normalize relative path: forward slashes in manifest
        $relPathNorm = $relPath.Replace('\', '/')
        if ($manifestFiles.ContainsKey($relPathNorm)) {
            Write-Host "[-] FAIL: duplicate manifest entry for $relPathNorm"
            $issues++
            continue
        }
        $manifestFiles[$relPathNorm] = $expectedHash

        $localRel = $relPathNorm.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
        $fullPath = Join-Path $refPath $localRel

        if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
            Write-Host "[-] MISSING: $relPathNorm"
            Write-Host "    Expected file listed in MANIFEST.sha256 was not found on disk."
            $issues++
            continue
        }

        $actualHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -eq $expectedHash) {
            Write-Host "[+] OK: $relPathNorm"
            $checked++
        } else {
            Write-Host "[-] MISMATCH: $relPathNorm"
            Write-Host "    Expected: $expectedHash"
            Write-Host "    Actual:   $actualHash"
            $issues++
        }
    } else {
        Write-Host "[-] FAIL: malformed line in MANIFEST.sha256: $rawLine"
        $issues++
    }
}

# Check for files present in tree but absent from manifest
$diskFiles = Get-ChildItem -LiteralPath $refPath -Recurse -File
foreach ($file in $diskFiles) {
    # Compute relative path from $refPath
    $relFromRoot = $file.FullName.Substring($refPath.Length)
    if ($relFromRoot.StartsWith('\') -or $relFromRoot.StartsWith('/')) {
        $relFromRoot = $relFromRoot.Substring(1)
    }
    $relFromRootNorm = $relFromRoot.Replace('\', '/')

    # MANIFEST.sha256 covers other files, not itself
    if ($relFromRootNorm -eq "MANIFEST.sha256") {
        continue
    }

    if (-not $manifestFiles.ContainsKey($relFromRootNorm)) {
        Write-Host "[-] UNTRACKED: $relFromRootNorm"
        Write-Host "    File is present in reference\isochronous\ but absent from MANIFEST.sha256."
        $issues++
    }
}

Write-Host ""
if ($issues -gt 0) {
    Write-Host "[-] ISOCHRONOUS REFERENCE MODIFIED: $issues issue(s) detected in reference\isochronous"
    Write-Host "    The isochronous reference evidence does not match MANIFEST.sha256."
    Write-Host "    The recorded measurement is not meant to change; regenerate MANIFEST.sha256 only for an intended update."
    exit 1
} else {
    Write-Host "[+] ISOCHRONOUS REFERENCE INTACT: reference\isochronous matches MANIFEST.sha256 exactly ($checked file(s) verified)"
    exit 0
}
