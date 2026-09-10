@echo off
rem Compile and run the descriptor self-test using the extracted EWDK toolchain.
rem No elevation, no installation: cl.exe and the SDK headers are used in place.
setlocal EnableDelayedExpansion

set "EWDK=C:\EWDK"
set "MSVC_ROOT=%EWDK%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
set "SDK=%EWDK%\Program Files\Windows Kits\10"
set "SDKVER=10.0.26100.0"

for /d %%d in ("%MSVC_ROOT%\*") do set "MSVC=%%d"
if not defined MSVC (
  echo ERROR: no MSVC toolset under "%MSVC_ROOT%"
  exit /b 1
)

set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"

set "HERE=%~dp0"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"

echo Toolset: %MSVC%

cl.exe /nologo /W4 /WX /Fe:"%OUT%\descriptor_selftest.exe" /Fo:"%OUT%\\" ^
   "%HERE%descriptor_selftest.c" "%HERE%..\src\common\usb_descriptors.c"
if errorlevel 1 (
  echo BUILD FAILED: descriptor_selftest
  exit /b 1
)

cl.exe /nologo /W4 /WX /Fe:"%OUT%\hci_selftest.exe" /Fo:"%OUT%\\" ^
   "%HERE%hci_selftest.c" "%HERE%..\src\driver\hci_stub.c"
if errorlevel 1 (
  echo BUILD FAILED: hci_selftest
  exit /b 1
)

cl.exe /nologo /W4 /WX /Fe:"%OUT%\qca_selftest.exe" /Fo:"%OUT%\\" ^
   "%HERE%qca_selftest.c" "%HERE%..\src\common\qca_tlv.c"
if errorlevel 1 (
  echo BUILD FAILED: qca_selftest
  exit /b 1
)

cl.exe /nologo /W4 /WX /Fe:"%OUT%\qca_fsm_selftest.exe" /Fo:"%OUT%\\" ^
   "%HERE%qca_fsm_selftest.c" "%HERE%..\src\common\qca_init_fsm.c" "%HERE%..\src\common\qca_tlv.c"
if errorlevel 1 (
  echo BUILD FAILED: qca_fsm_selftest
  exit /b 1
)

echo.
"%OUT%\descriptor_selftest.exe"
if errorlevel 1 exit /b 1
echo.
"%OUT%\hci_selftest.exe"
if errorlevel 1 exit /b 1
echo.
"%OUT%\qca_selftest.exe"
if errorlevel 1 exit /b 1
echo.
"%OUT%\qca_fsm_selftest.exe"
exit /b %errorlevel%
