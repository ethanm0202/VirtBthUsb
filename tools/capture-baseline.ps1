<#
    capture-baseline.ps1 - record the restoration target, and back up the vendor radio driver.

    The driver takes the physical radio from its vendor driver. Before that happens, the vendor's
    exact ownership of ACPI\QCOM2066 and a byte-for-byte copy of its driver package are saved,
    so the radio can be handed back even if the vendor's DriverStore entry is gone.

    WHAT IT WRITES
      recovery/baseline/baseline.json          observed machine state
      recovery/baseline/vendor-qcbtuart/       verified copy of the vendor driver package
      recovery/baseline/MANIFEST.sha256        hashes of everything above

    It never modifies anything else, and it refuses to clobber an existing baseline without -Force,
    because a baseline captured earlier is more valuable than a fresh one.

    EXIT CODES  0 captured   1 refused/failed   2 precondition missing
#>
[CmdletBinding()]
param(
    [switch] $Force,   # archive the existing baseline and capture a brand new one
    [switch] $Show     # print the existing baseline and exit; changes nothing
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'deck-state.ps1')

function Write-Ok  ([string] $m) { Write-Host "[+] $m" -ForegroundColor Green }
function Write-Info([string] $m) { Write-Host "[*] $m" -ForegroundColor Gray }
function Write-Warn2([string] $m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Write-Bad ([string] $m) { Write-Host "[-] $m" -ForegroundColor Red }

Write-Host ''
Write-Host 'Baseline capture - record restoration target state'

# Keep a transcript: when this runs in its own elevated console, the exit code is otherwise all
# that remains. Logs live outside the baseline directory so they never change MANIFEST.sha256
# and are not removed by -Force archiving.
$script:captureLogDir = Join-Path $script:DeckRoot 'recovery\baseline-logs'
New-Item -ItemType Directory -Force -Path $script:captureLogDir | Out-Null
$script:captureLog = Join-Path $script:captureLogDir ("capture-{0}-{1}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID)
try { Start-Transcript -Path $script:captureLog -ErrorAction Stop | Out-Null; $script:transcriptOn = $true }
catch { $script:transcriptOn = $false; Write-Host "[!] transcript unavailable: $($_.Exception.Message)" -ForegroundColor Yellow }
Write-Host "  log: $script:captureLog"

function Write-Manifest {
    $manifestPath = Join-Path $DeckBaselineDir 'MANIFEST.sha256'
    if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath -Force }
    $lines = @()
    foreach ($f in Get-ChildItem -LiteralPath $DeckBaselineDir -Recurse -File | Sort-Object FullName) {
        $rel = $f.FullName.Substring($DeckBaselineDir.Length).TrimStart('\', '/').Replace('\', '/')
        $lines += "$((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $rel"
    }
    [IO.File]::WriteAllText($manifestPath, (($lines -join "`n") + "`n"))
    return $lines.Count
}

function Show-Baseline($b) {
    Write-Info "captured      : $($b.capturedUtc) UTC (elevated: $($b.capturedElevated))"
    if ($b.enrichedUtc) { Write-Info "last enriched : $($b.enrichedUtc) UTC" }
    Write-Info "test signing  : $($b.testSigning)"
    Write-Info "HVCI          : $($b.hvciEnabled)"
    Write-Info "Secure Boot   : $($b.secureBoot)"
    Write-Info "BitLocker     : $($b.bitLocker)"
    Write-Info "radio         : $($b.qcom2066.instanceId) service=$($b.qcom2066.service) status=$($b.qcom2066.status)"
    Write-Info "paired devices: $(@($b.pairedDevices).Count)"
    Write-Info "vendor backup : $(@($b.vendorBackup.files).Count) file(s) in $($b.vendorBackup.directory)"
}

$existing = $null
if (Test-Path -LiteralPath $DeckBaselineJson) {
    try { $existing = Get-Content -LiteralPath $DeckBaselineJson -Raw | ConvertFrom-Json }
    catch { Write-Warn2 "Existing baseline at $DeckBaselineJson is unparseable: $($_.Exception.Message)" }
}

if ($Show) {
    if (-not $existing) { Write-Bad 'No baseline to show. Run tools\capture-baseline.ps1 first.'; exit 1 }
    Write-Ok "Baseline at $DeckBaselineJson"
    Show-Baseline $existing
    exit 0
}

# An existing baseline is never silently discarded: it may be closer to the pre-project state than
# anything captured today. But refusing outright was wrong, because the common case is exactly this
# one - a non-elevated capture that left fields unreadable, followed by an elevated re-run.
if ($existing -and -not $Force) {
    Write-Ok "Baseline already exists, captured $($existing.capturedUtc) UTC (elevated: $($existing.capturedElevated))."
    Show-Baseline $existing
    Write-Host ''

    # Only fields that are genuinely unreadable get re-read; known values are never overwritten,
    # so an older, truer observation cannot be replaced by a newer one.
    $candidates = @(
        @{ Key = 'testSigning'; Current = $existing.testSigning; Read = { Get-DeckTestSigning } }
        @{ Key = 'secureBoot';  Current = $existing.secureBoot;  Read = { Get-DeckSecureBoot } }
        @{ Key = 'bitLocker';   Current = $existing.bitLocker;   Read = { Get-DeckBitLocker } }
        @{ Key = 'hvciEnabled'; Current = $existing.hvciEnabled; Read = { Get-DeckHvci } }
    )
    $improved = @()
    foreach ($c in $candidates) {
        if ("$($c.Current)" -ne 'unknown') { continue }
        $now = & $c.Read
        if ("$now" -ne 'unknown') { $improved += [pscustomobject]@{ Key = $c.Key; From = $c.Current; To = $now } }
    }

    if ($improved.Count -eq 0) {
        $stillUnknown = @($candidates | Where-Object { "$($_.Current)" -eq 'unknown' } | ForEach-Object { $_.Key })
        if ($stillUnknown.Count -eq 0) {
            Write-Ok 'Nothing to add: every field is already recorded. The baseline is complete.'
        } else {
            Write-Warn2 "Still unreadable here: $($stillUnknown -join ', ')."
            if (-not (Test-DeckElevated)) { Write-Warn2 'Re-run this elevated (right-click, Run as administrator) to fill them in.' }
            else { Write-Warn2 'Even elevated these could not be read; the values may not exist on this machine.' }
        }
        Write-Host ''
        Write-Info 'Options: -Show to print this again, -Force to archive this baseline and capture a new one.'
        exit 0
    }

    foreach ($i in $improved) {
        $existing.$($i.Key) = $i.To
        Write-Ok "enriched $($i.Key): unknown -> $($i.To)"
    }
    $enrichLog = @()
    if ($existing.PSObject.Properties.Name -contains 'enrichmentLog' -and $existing.enrichmentLog) { $enrichLog = @($existing.enrichmentLog) }
    $enrichLog += [ordered]@{
        utc      = (Get-Date).ToUniversalTime().ToString('o')
        elevated = Test-DeckElevated
        fields   = @($improved | ForEach-Object { "$($_.Key)=$($_.To)" })
    }
    $existing | Add-Member -NotePropertyName 'enrichedUtc' -NotePropertyValue ((Get-Date).ToUniversalTime().ToString('o')) -Force
    $existing | Add-Member -NotePropertyName 'enrichmentLog' -NotePropertyValue $enrichLog -Force
    $existing | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $DeckBaselineJson -Encoding UTF8
    $n = Write-Manifest
    Write-Ok "updated $DeckBaselineJson and MANIFEST.sha256 ($n file(s))"
    Write-Info 'capturedUtc is unchanged: this is the same baseline, with previously unreadable fields filled in.'
    exit 0
}

if ($existing -and $Force) {
    # -Force never deletes: the previous baseline is archived first, so an earlier and possibly
    # truer snapshot always remains on disk.
    $stamp = ($existing.capturedUtc -replace '[^0-9A-Za-z]', '-')
    if (-not $stamp) { $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss') }
    $archive = Join-Path $script:DeckRoot "recovery\baseline-archive\$stamp"
    New-Item -ItemType Directory -Force -Path $archive | Out-Null
    Copy-Item -Path (Join-Path $DeckBaselineDir '*') -Destination $archive -Recurse -Force
    Write-Ok "archived the previous baseline to $archive"
    Remove-Item -LiteralPath $DeckBaselineDir -Recurse -Force
}

$elevated = Test-DeckElevated
if (-not $elevated) { Write-Warn2 'Not elevated: values requiring elevation will be recorded as "unknown".' }

New-Item -ItemType Directory -Force -Path $DeckBaselineDir | Out-Null

# --- pre-project artifacts -------------------------------------------------------------------
$preProject = @()
foreach ($p in @($DeckDriversBefore, $DeckBtBefore, $DeckCertFile, $DeckBcdBackup)) {
    $preProject += [pscustomobject]@{
        path    = (Resolve-Path -LiteralPath $p -ErrorAction SilentlyContinue | Select-Object -First 1).Path
        name    = Split-Path -Leaf $p
        present = [bool](Test-Path -LiteralPath $p)
    }
}
foreach ($a in $preProject) {
    if ($a.present) { Write-Ok "pre-project artifact: $($a.name)" } else { Write-Warn2 "pre-project artifact MISSING: $($a.name)" }
}

# --- packages ---------------------------------------------------------------------------------
$staged = Get-DeckStagedPackages
$ours   = @($staged | Where-Object { $_.IsOurs })
$stock  = Get-DeckStockPackages
Write-Info "staged packages: $($staged.Count) total, $($ours.Count) from this project, $($stock.Count) recorded in drivers-before.txt"

# --- radio ------------------------------------------------------------------------------------
$node  = Get-DeckDevice $RadioAcpiId
$child = Get-DeckDevice $RadioChildLike -Like
if ($node) { Write-Ok "radio node: $($node.InstanceId) service=$($node.Service) status=$($node.Status)" }
else { Write-Bad "radio node $RadioAcpiId is not present - capture will record it as absent" }

# --- certificate thumbprints ------------------------------------------------------------------
$thumbs = @()
if (Test-Path -LiteralPath $DeckCertFile) {
    try {
        $thumbs += ([Security.Cryptography.X509Certificates.X509Certificate2]::new($DeckCertFile)).Thumbprint
        Write-Ok "test certificate thumbprint read from $(Split-Path -Leaf $DeckCertFile)"
    } catch { Write-Warn2 "could not read $DeckCertFile : $($_.Exception.Message)" }
}

# --- vendor package backup --------------------------------------------------------------------
$vendorFiles = @()
if (-not (Test-Path -LiteralPath $VendorStoreDir)) {
    Write-Bad "vendor package directory not found: $VendorStoreDir"
    Write-Bad 'Cannot create the radio-driver insurance copy. Fix this before taking over the radio.'
    exit 2
}
New-Item -ItemType Directory -Force -Path $VendorBackupDir | Out-Null
foreach ($f in Get-ChildItem -LiteralPath $VendorStoreDir -File) {
    $dest = Join-Path $VendorBackupDir $f.Name
    Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
    $srcHash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    $dstHash = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    if ($srcHash -ne $dstHash) {
        Write-Bad "copy verification FAILED for $($f.Name)"
        exit 1
    }
    $vendorFiles += [pscustomobject]@{ name = $f.Name; bytes = $f.Length; sha256 = $srcHash }
}
Write-Ok "vendor package copied and hash-verified: $($vendorFiles.Count) file(s)"

# --- assemble ---------------------------------------------------------------------------------
$paired = Get-DeckPairedDevices
$os = Get-CimInstance Win32_OperatingSystem
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'

$baseline = [ordered]@{
    capturedUtc      = (Get-Date).ToUniversalTime().ToString('o')
    capturedElevated = $elevated
    osCaption        = $os.Caption
    osVersion        = $os.Version
    osDisplayVersion = $cv.DisplayVersion
    osBuildLab       = $cv.BuildLabEx
    testSigning      = Get-DeckTestSigning
    hvciEnabled      = Get-DeckHvci
    secureBoot       = Get-DeckSecureBoot
    bitLocker        = Get-DeckBitLocker
    qcom2066         = [ordered]@{
        instanceId      = if ($node) { $node.InstanceId } else { $null }
        service         = if ($node) { $node.Service } else { $null }
        status          = if ($node) { "$($node.Status)" } else { 'absent' }
        childInstanceId = if ($child) { $child.InstanceId } else { $null }
        childService    = if ($child) { $child.Service } else { $null }
        expectedService = $VendorService
        vendorStoreDir  = $VendorStoreDir
    }
    pairedDevices       = @($paired | ForEach-Object { [ordered]@{ name = $_.Name; instanceId = $_.InstanceId; status = "$($_.Status)" } })
    deckPackages        = @($ours | ForEach-Object { [ordered]@{ published = $_.Published; originalName = $_.OriginalName; providerName = $_.Provider; version = $_.Version } })
    foreignPackagesCount = @($staged | Where-Object { -not $_.IsOurs }).Count
    stockPackages       = @($stock | ForEach-Object { [ordered]@{ published = $_.Published; originalName = $_.OriginalName; providerName = $_.Provider } })
    deckServices        = @($DeckServices | ForEach-Object {
        $k = "HKLM:\SYSTEM\CurrentControlSet\Services\$_"
        $p = Get-ItemProperty -LiteralPath $k -ErrorAction SilentlyContinue
        [ordered]@{ name = $_; present = [bool](Test-Path -LiteralPath $k); startType = $p.Start; imagePath = $p.ImagePath }
    })
    deckDevnodes        = @($DeckDevnodes | ForEach-Object {
        $d = Get-DeckDevice $_
        [ordered]@{ instanceId = $_; present = [bool]$d; status = if ($d) { "$($d.Status)" } else { 'absent' } }
    })
    testCertThumbprints = @($thumbs)
    preProjectArtifacts = @($preProject | ForEach-Object { [ordered]@{ name = $_.name; path = $_.path; present = $_.present } })
    vendorBackup        = [ordered]@{ directory = $VendorBackupDir; files = $vendorFiles }
}

$baseline | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $DeckBaselineJson -Encoding UTF8
Write-Ok "wrote $DeckBaselineJson"

# --- manifest ---------------------------------------------------------------------------------
Write-Ok "wrote MANIFEST.sha256 ($(Write-Manifest) file(s))"

Write-Host ''
Write-Ok 'BASELINE CAPTURED.'
Write-Info "restoration target: $RadioAcpiId owned by $VendorService, $(@($paired).Count) paired device(s)"
if (-not $elevated) {
    Write-Warn2 'testSigning/secureBoot/bitLocker were unreadable without elevation.'
    Write-Warn2 'Just re-run this elevated: it will fill those fields in on the SAME baseline,'
    Write-Warn2 'keeping the original capturedUtc. No -Force needed.'
}
exit 0
