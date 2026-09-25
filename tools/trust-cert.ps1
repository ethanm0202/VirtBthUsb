<#
    trust-cert.ps1 - trust recovery\DeckBtUsbTestCert.cer for the driver package.
    Never changes boot flags, packages, devices, the radio, or pairings.
    The current boot must already have test signing active; BCD configuration alone
    is not proof that the required reboot happened. tools\uninstall.ps1 removes this cert.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $DryRun,
    [switch] $Worker
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$certFile = Join-Path $root 'recovery\DeckBtUsbTestCert.cer'
$catalog = Join-Path $root "src\driver\x64\$Configuration\deckbtusb\deckbtusb.cat"

# Isolate certificate-store/native calls too: a stuck trust provider cannot trap the console.
if (-not $Worker) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PSHOME 'powershell.exe'
    $start.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $PSCommandPath + '" -Worker -Configuration ' + $Configuration
    if ($DryRun) { $start.Arguments += ' -DryRun' }
    $start.UseShellExecute = $false
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        if (-not $process.WaitForExit(60000)) {
            try { $process.Kill() } catch { }
            Write-Host 'PREPARE FAILED: certificate preparation exceeded 60s. No further operations attempted. tools\uninstall.ps1 removes the test certificate if an import completed.' -ForegroundColor Red
            exit 1
        }
        exit $process.ExitCode
    } finally { $process.Dispose() }
}

try {
    Write-Host 'Trust test certificate: exact project certificate only; no boot, PnP, package, or radio changes.'
    Write-Host 'Reversal: tools\session.ps1 -Uninstall removes this certificate from both stores.'
    if (-not $DryRun) {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        if (-not ([Security.Principal.WindowsPrincipal]::new($id)).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
            throw 'Administrator rights required. Run from an elevated PowerShell console; this helper does not elevate itself.'
        }
    }
    if (-not (Test-Path -LiteralPath $certFile -PathType Leaf)) { throw "Exact existing certificate is missing: $certFile. No replacement certificate will be selected or generated." }
    if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) { throw "Package catalog is missing: $catalog" }
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certFile)
    try {
        $thumbprint = $cert.Thumbprint
        Write-Host "Certificate: $certFile"
        Write-Host "Exact thumbprint: $thumbprint"
        $signature = Get-AuthenticodeSignature -LiteralPath $catalog
        if (-not $signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $thumbprint) {
            throw 'Package catalog is not signed by the exact recovery certificate. Refusing to import a different certificate; rebuild/sign with the existing project certificate.'
        }
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DeckCodeIntegrity {
    [StructLayout(LayoutKind.Sequential)]
    public struct CodeIntegrityInformation {
        public uint Length;
        public uint Options;
    }
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int informationClass,
        ref CodeIntegrityInformation information, uint length, out uint returnLength);
}
'@
        $ci = [DeckCodeIntegrity+CodeIntegrityInformation]::new()
        $ci.Length = 8
        $returned = [uint32]0
        $status = [DeckCodeIntegrity]::NtQuerySystemInformation(103, [ref]$ci, 8, [ref]$returned)
        if ($status -ne 0 -or $returned -lt 8) { throw ('Cannot verify active boot code-integrity state (NTSTATUS=0x{0:X8}). No certificate import attempted.' -f $status) }
        if (($ci.Options -band 2) -eq 0) {
            throw 'Test signing is NOT ACTIVE in the current boot. This tool will not enable it. Run tools\session.ps1 -Prepare, reboot, then run this tool again.'
        }
        Write-Host 'Current boot: test signing ACTIVE (kernel code-integrity options verified).'
        foreach ($store in 'Root', 'TrustedPublisher') {
            $path = "Cert:\LocalMachine\$store\$thumbprint"
            if (Test-Path -LiteralPath $path) {
                Write-Host "ALREADY PRESENT: LocalMachine\$store\$thumbprint"
            } elseif ($DryRun) {
                Write-Host "DRY-RUN: would import only $thumbprint into LocalMachine\$store"
            } else {
                Import-Certificate -FilePath $certFile -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            }
            if (-not $DryRun) {
                $stored = Get-Item -LiteralPath $path
                if ($stored.Thumbprint -ne $thumbprint) { throw "Certificate verification failed in LocalMachine\$store" }
                Write-Host "VERIFIED: LocalMachine\$store\$thumbprint"
            }
        }
        if ($DryRun) {
            Write-Host "DRY-RUN complete: certificate stores unchanged. Current catalog status: $($signature.Status). Live run must verify Valid after trust import."
        } else {
            $signature = Get-AuthenticodeSignature -LiteralPath $catalog
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $thumbprint) {
                throw "Package catalog still not Valid after exact-certificate import: $($signature.Status). tools\session.ps1 -Uninstall removes the imported certificate."
            }
            Write-Host 'PREPARE PASS: exact certificate verified in both stores; package catalog Authenticode Status=Valid.'
            Write-Host 'Next: tools\session.ps1 -Identify (or -Probe / -Bridge).'
        }
    } finally { $cert.Dispose() }
    exit 0
} catch {
    Write-Host "PREPARE REFUSAL: $($_.Exception.Message)" -ForegroundColor Red
    exit 2
}
