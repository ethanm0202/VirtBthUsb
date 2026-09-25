<#
    vendor-log.ps1 - Configure the vendor Qualcomm driver (qcbtuart.sys, service QcBluetooth) to log its
    own UART/SoC bring-up. Names come from static analysis of qcbtuart.sys; see docs\QCA2066.md.

    Log location: C:\Windows\ServiceState\QcBluetooth\Data\btlog.log.
    The driver opens its data directory with IoGetDriverDirectory(DriverDirectoryData) and only
    falls back to the hard-coded \DosDevices\C:\Data\ProgramData\btlog.log when that API is absent.

      -Arm     Save the original Parameters values and set BtLogEnabled=1 and BtLogAppend=1. Read
               only at driver load; QcBluetooth removal is vetoed while Bluetooth devices are
               open, so logging starts at the next boot. Elevated.
      -Collect Copy the log (read/write sharing: the running driver holds it open) to
               tools\_build\vendorlog\<stamp>\ and print the SoC-init lines. Elevated
               (ServiceState is SYSTEM/Administrators only).
      -Disarm  Restore the saved values; logging stops at the next boot. -RemoveLog also deletes
               the log (after that boot) and the C:\Data\ProgramData directory an earlier version
               of this tool created. Elevated.

    Never installs, uninstalls, restarts, or rebinds a device, and never touches pairings. The raw
    log contains HCI traffic of paired devices; it stays in git-ignored tools\_build.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, ParameterSetName = 'Arm')] [switch] $Arm,
    [Parameter(Mandatory, ParameterSetName = 'Collect')] [switch] $Collect,
    [Parameter(Mandatory, ParameterSetName = 'Disarm')] [switch] $Disarm,
    [Parameter(ParameterSetName = 'Disarm')] [switch] $RemoveLog
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')

$paramKey  = "HKLM:\SYSTEM\CurrentControlSet\Services\$VendorService\Parameters"
$names     = @('BtLogEnabled', 'BtLogAppend')
$logFile   = Join-Path $env:SystemRoot "ServiceState\$VendorService\Data\btlog.log"
$legacyDir = 'C:\Data\ProgramData'
$stateDir  = Join-Path $PSScriptRoot '_build\vendorlog'
$stateFile = Join-Path $stateDir 'armed-original-params.json'

if (-not (Test-DeckElevated)) { Write-Host 'REFUSAL: run elevated.'; exit 2 }

if ($Arm) {
    New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    if (-not (Test-Path -LiteralPath $stateFile)) {
        $props = Get-ItemProperty -LiteralPath $paramKey
        $orig = [ordered]@{ DataDirExisted = (Test-Path -LiteralPath 'C:\Data'); LogDirExisted = (Test-Path -LiteralPath $legacyDir) }
        foreach ($n in $names) {
            $orig[$n] = if ($props.PSObject.Properties.Name -contains $n) { [int]$props.$n } else { $null }
        }
        ($orig | ConvertTo-Json) | Set-Content -LiteralPath $stateFile -Encoding ASCII
    }
    foreach ($n in $names) {
        New-ItemProperty -LiteralPath $paramKey -Name $n -PropertyType DWord -Value 1 -Force | Out-Null
    }
    $flags = [uint32]('0x{0:X8}' -f (Get-ItemProperty -LiteralPath $paramKey).DebugFlags)
    if (($flags -band 0x4000) -eq 0) { throw "DebugFlags lacks 0x4000; the BTLOG sink would stay silent." }
    Write-Host "ARMED: QcBluetooth BtLogEnabled=1 BtLogAppend=1. Takes effect at next boot; log $logFile."
    Write-Host "Originals saved to $stateFile."
    exit 0
}

if ($Collect) {
    if (-not (Test-Path -LiteralPath $logFile)) {
        Write-Host "NO LOG: $logFile absent. (Arm state: $(Test-Path -LiteralPath $stateFile))"
        exit 3
    }
    $out = Join-Path $stateDir (Get-Date -Format 'yyyyMMdd-HHmmss')
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $dest = Join-Path $out 'btlog.log'
    $src = [IO.File]::Open($logFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $dst = [IO.File]::Create($dest)
        try { $src.CopyTo($dst) } finally { $dst.Dispose() }
    } finally { $src.Dispose() }
    Write-Host "COLLECTED: $((Get-Item -LiteralPath $dest).Length) bytes -> $dest"
    $pattern = 'D0Entry|D0Exit|modem status|Probing|comm status|Product_ID|It is H|build_info:|boardID|Nvm file path|SetNVMBaudRate|Soc init|FirmwareInitialize fail|Retry|EDL reset|baudrate set|HCI CMD: (00 FC 01|48 FC|40 FC|03 0C|17 FC)|HCI EVENT: 0E (12|08|04|05 01 17)'
    Select-String -LiteralPath $dest -Pattern $pattern | Select-Object -Last 120 | ForEach-Object { $_.Line }
    exit 0
}

if ($Disarm) {
    if (-not (Test-Path -LiteralPath $stateFile)) { throw "No saved originals at $stateFile; refusing to guess stock values." }
    $orig = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
    foreach ($n in $names) {
        if ($null -eq $orig.$n) {
            Remove-ItemProperty -LiteralPath $paramKey -Name $n -ErrorAction SilentlyContinue
        } else {
            New-ItemProperty -LiteralPath $paramKey -Name $n -PropertyType DWord -Value ([int]$orig.$n) -Force | Out-Null
        }
    }
    Write-Host "DISARMED: QcBluetooth logging parameters restored ($(($names | ForEach-Object { "$_=$(if ($null -eq $orig.$_) { '<absent>' } else { $orig.$_ })" }) -join ' ')). Logging stops at next boot."
    if (-not $orig.LogDirExisted -and (Test-Path -LiteralPath $legacyDir)) { Remove-Item -LiteralPath $legacyDir -Recurse -Force }
    if (-not $orig.DataDirExisted -and (Test-Path -LiteralPath 'C:\Data') -and -not @(Get-ChildItem 'C:\Data' -Force).Count) {
        Remove-Item -LiteralPath 'C:\Data' -Force
    }
    if ($RemoveLog) {
        try {
            if (Test-Path -LiteralPath $logFile) { Remove-Item -LiteralPath $logFile -Force }
            Remove-Item -LiteralPath $stateFile -Force
            Write-Host 'LOG REMOVED.'
        } catch {
            Write-Host "Log still held open by the running driver; rerun -Disarm -RemoveLog after the next boot. ($($_.Exception.Message))"
        }
    }
    exit 0
}
