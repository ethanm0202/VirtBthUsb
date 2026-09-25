@echo off
rem check-isoc-reference.cmd - assert that the isochronous reference evidence has not been modified.
rem
rem reference\isochronous\ is the project's regression target for isochronous transfer.
rem This script verifies that every file matches MANIFEST.sha256 exactly, no tracked files are
rem missing, and no untracked files are present in the reference tree.
rem
rem Exit 0 = intact. Exit 1 = drift / tampering / missing / untracked file.
rem No elevation, read-only.
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" (
    echo [-] FAIL: powershell.exe not found at "%PS%"
    exit /b 1
)

"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-isoc-reference.ps1" %*
set "RC=%ERRORLEVEL%"
exit /b %RC%
