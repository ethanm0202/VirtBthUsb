@echo off
rem Build the DeckBtUsb driver package with the extracted EWDK. No elevation, no installation.
rem   tools\build.cmd            -> Release
rem   tools\build.cmd Debug      -> Debug
setlocal

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

if not defined EWDK set "EWDK=C:\EWDK"
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
  echo ERROR: EWDK not found at %EWDK%
  exit /b 1
)

set "PROJ=%~dp0..\src\driver\deckbtusb.vcxproj"
set "PROJ2=%~dp0..\src\filter\deckbtflt.vcxproj"
set "SHIPPING_OUT=%~dp0..\src\driver\x64\%CFG%\deckbtusb"

set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
echo Staging firmware...
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0stage-firmware.ps1" -Configuration "%CFG%"
if errorlevel 1 (
  echo.
  echo BUILD FAILED: firmware staging failed
  exit /b 1
)

rem SetupBuildEnv.cmd sets MSVC + SDK + WDK vars in the CURRENT shell.
rem LaunchBuildEnv.cmd is unusable from a script: it spawns an interactive `cmd /k`.
call "%EWDK%\BuildEnv\SetupBuildEnv.cmd" >nul
msbuild "%PROJ%" /nologo /v:minimal /t:Build /p:Configuration=%CFG% /p:Platform=x64
if errorlevel 1 (
  echo.
  echo BUILD FAILED: deckbtusb
  exit /b 1
)
msbuild "%PROJ2%" /nologo /v:minimal /t:Build /p:Configuration=%CFG% /p:Platform=x64

set "RC=%errorlevel%"
if not "%RC%"=="0" (
  echo.
  echo BUILD FAILED rc=%RC%
  exit /b %RC%
)

rem Verify the built shipping package contains every file declared in [SourceDisksFiles].
"%PS%" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $infPath = Join-Path $env:SHIPPING_OUT 'deckbtusb.inf'; if (-not (Test-Path -LiteralPath $infPath)) { throw 'Generated shipping INF missing: ' + $infPath }; $inSection = $false; $requiredFiles = [System.Collections.Generic.List[string]]::new(); foreach ($line in Get-Content -LiteralPath $infPath) { $trimmed = $line.Trim(); if ($trimmed.StartsWith(';') -or [string]::IsNullOrWhiteSpace($trimmed)) { continue }; if ($trimmed.StartsWith('[') -and $trimmed.EndsWith(']')) { $section = $trimmed.Substring(1, $trimmed.Length - 2).Trim(); if ($section -like 'SourceDisksFiles*') { $inSection = $true } else { $inSection = $false }; continue }; if ($inSection) { $parts = $trimmed -split '=', 2; $file = $parts[0].Trim(); if ($file) { $requiredFiles.Add($file) } } }; if ($requiredFiles.Count -eq 0) { throw 'No files found in [SourceDisksFiles] in ' + $infPath }; $missing = @(); foreach ($f in $requiredFiles) { $p = Join-Path $env:SHIPPING_OUT $f; if (-not (Test-Path -LiteralPath $p)) { $missing += $f } }; if ($missing.Count -gt 0) { throw ('Shipping package missing required file(s) declared in [SourceDisksFiles]: ' + ($missing -join ', ')) }; Write-Host ('Shipping package [SourceDisksFiles] consistency verified: ' + ($requiredFiles -join ', ')) } catch { Write-Host $_; exit 1 }"
if errorlevel 1 (
  echo.
  echo BUILD FAILED: shipping package file consistency verification
  exit /b 1
)
echo.
echo Build output:
dir /b "%~dp0..\src\driver\x64\%CFG%\deckbtusb" 2>nul
dir /b "%~dp0..\src\filter\x64\%CFG%\deckbtflt" 2>nul
exit /b 0
