@echo off
rem Compile and run all self-test suites using the extracted EWDK toolchain.
rem No elevation, no installation: cl.exe and the SDK headers are used in place.
setlocal EnableDelayedExpansion

if not defined EWDK set "EWDK=C:\EWDK"
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
set "ROOT=%HERE%..\"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"

rem Firmware for the QCA suites: the newest installed vendor package, unless QCA_FW_DIR is set.
rem Same rule as tools\stage-firmware.ps1. Without either, the suites read the copy the build stages.
set "FW_REPO=%SystemRoot%\System32\DriverStore\FileRepository"
if not defined QCA_FW_DIR (
  for /f "delims=" %%d in ('dir /b /ad /o-d "%FW_REPO%\qcbtuart.inf_amd64_*" 2^>nul') do (
    if not defined QCA_FW_DIR set "QCA_FW_DIR=%FW_REPO%\%%d"
  )
)
if defined QCA_FW_DIR echo Firmware: %QCA_FW_DIR%

pushd "%ROOT%"

echo Toolset: %MSVC%

set SUITES=^
 "descriptor_selftest|tools\descriptor_selftest.c src\common\usb_descriptors.c"^
 "hci_selftest|tools\hci_selftest.c src\driver\hci_stub.c"^
 "qca_selftest|tools\qca_selftest.c src\common\qca_tlv.c"^
 "qca_fsm_selftest|tools\qca_fsm_selftest.c src\common\qca_init_fsm.c src\common\qca_tlv.c"^
 "nvm_selftest|tools\nvm_selftest.c src\common\qca_tlv.c src\common\qca_init_fsm.c"^
 "tlv_segment_selftest|tools\tlv_segment_selftest.c src\common\qca_tlv.c"^
 "identify_selftest|tools\identify_selftest.c src\common\qca_identify.c src\common\qca_tlv.c"^
 "h4_selftest|tools\h4_selftest.c src\common\h4_codec.c"^
 "bridge_selftest|tools\bridge_selftest.c src\common\hci_bridge.c"^
 "sco_usb_selftest|tools\sco_usb_selftest.c src\common\sco_usb.c"^
 "sco_route_selftest|tools\sco_route_selftest.c src\common\sco_route.c"^
 "isotest_selftest|tools\isotest_selftest.c src\isotest\descriptors.c src\common\usb_descriptors.c"

echo.
echo Building test suites...
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    for /f "tokens=1*" %%p in ("%%b") do set "PRIMARY=%%p"
    if not exist "!PRIMARY!" (
      echo [skip] %%a ^(!PRIMARY! not present yet^)
      set "BUILT_%%a=skipped"
      set "PASSED_%%a=skipped"
    ) else (
      set "OBJDIR=%OUT%\%%a"
      if not exist "!OBJDIR!" mkdir "!OBJDIR!"
      cl.exe /nologo /W4 /WX /Fe:"%OUT%\%%a.exe" /Fo:"!OBJDIR!\\" %%b
      if errorlevel 1 (
        echo BUILD FAILED: %%a
        set "BUILT_%%a=FAIL"
        set "PASSED_%%a=-"
      ) else (
        set "BUILT_%%a=yes"
        set "PASSED_%%a=pending"
      )
    )
  )
)

echo.
echo Running test suites...
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    if "!BUILT_%%a!"=="yes" (
      echo.
      "%OUT%\%%a.exe"
      if errorlevel 1 (
        set "PASSED_%%a=FAIL"
      ) else (
        set "PASSED_%%a=yes"
      )
    )
  )
)

rem Verify the isochronous reference manifest separately from the C test suites.
echo.
echo Checking isochronous reference integrity...
call "%HERE%check-isoc-reference.cmd"
if errorlevel 1 (
  set "BUILT_isoc_reference=n/a"
  set "PASSED_isoc_reference=FAIL"
) else (
  set "BUILT_isoc_reference=n/a"
  set "PASSED_isoc_reference=yes"
)

echo.
echo Running operator selftest...
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%operator-selftest.ps1"
if errorlevel 1 (
  set "BUILT_operator=n/a"
  set "PASSED_operator=FAIL"
) else (
  set "BUILT_operator=n/a"
  set "PASSED_operator=yes"
)

echo.
echo Running service selftest...
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%service-selftest.ps1"
if errorlevel 1 (
  set "BUILT_service=n/a"
  set "PASSED_service=FAIL"
) else (
  set "BUILT_service=n/a"
  set "PASSED_service=yes"
)

echo.
echo Running recovery safety selftest...
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%recovery-selftest.ps1"
if errorlevel 1 (
  set "BUILT_recovery=n/a"
  set "PASSED_recovery=FAIL"
) else (
  set "BUILT_recovery=n/a"
  set "PASSED_recovery=yes"
)

echo.
echo Running UART identify failure-path harness selftest...
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%uart-identify-selftest.ps1"
if errorlevel 1 (
  set "BUILT_uart_identify=n/a"
  set "PASSED_uart_identify=FAIL"
) else (
  set "BUILT_uart_identify=n/a"
  set "PASSED_uart_identify=yes"
)

set /a FAILED_SUITES=0
set /a SKIPPED_SUITES=0
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    set "FAILED=0"
    if "!BUILT_%%a!"=="FAIL" set "FAILED=1"
    if "!PASSED_%%a!"=="FAIL" set "FAILED=1"
    if "!FAILED!"=="1" set /a FAILED_SUITES+=1
    if "!BUILT_%%a!"=="skipped" set /a SKIPPED_SUITES+=1
  )
)
if "!PASSED_isoc_reference!"=="FAIL" set /a FAILED_SUITES+=1
if "!PASSED_operator!"=="FAIL" set /a FAILED_SUITES+=1
if "!PASSED_service!"=="FAIL" set /a FAILED_SUITES+=1
if "!PASSED_recovery!"=="FAIL" set /a FAILED_SUITES+=1
if "!PASSED_uart_identify!"=="FAIL" set /a FAILED_SUITES+=1

echo.
echo suite                     ^| built   ^| passed
echo --------------------------+---------+--------
for %%E in (%SUITES%) do (
  for /f "tokens=1* delims=|" %%a in (%%E) do (
    set "NAME=%%a                         "
    set "NAME=!NAME:~0,25!"
    set "B=!BUILT_%%a!       "
    set "B=!B:~0,7!"
    set "P=!PASSED_%%a!      "
    set "P=!P:~0,7!"
    echo !NAME! ^| !B! ^| !P!
  )
)
for %%R in (isoc_reference operator service recovery uart_identify) do (
  set "NAME=%%R                         "
  set "NAME=!NAME:~0,25!"
  set "B=!BUILT_%%R!       "
  set "B=!B:~0,7!"
  set "P=!PASSED_%%R!       "
  set "P=!P:~0,7!"
  echo !NAME! ^| !B! ^| !P!
)

echo.
if !FAILED_SUITES! equ 0 (
  rem A skipped suite is not a passing suite. Absent source must never be quotable as coverage:
  rem the whole point of the table-driven loop is that suites appear as their tasks land, so the
  rem exit code stays 0, but the verdict line has to say out loud what was not exercised.
  if !SKIPPED_SUITES! equ 0 (
    echo SELFTEST PASSED
  ) else (
    echo SELFTEST PASSED - but !SKIPPED_SUITES! suite^(s^) SKIPPED ^(source absent^): NOT full coverage
  )
  popd
  exit /b 0
) else (
  echo SELFTEST FAILED: !FAILED_SUITES! suite^(s^)
  popd
  exit /b 1
)
