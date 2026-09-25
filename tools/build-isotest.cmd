@echo off
rem build-isotest.cmd - Build and test-sign the IsoTest driver and harness.
rem Uses extracted EWDK. No elevation, no installation, no collision with shipping driver.
rem
rem Usage:
rem   tools\build-isotest.cmd            -> Release
rem   tools\build-isotest.cmd Debug      -> Debug
setlocal

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

if not defined EWDK set "EWDK=C:\EWDK"
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
    echo ERROR: EWDK not found at %EWDK%
    exit /b 1
)

set "ROOT=%~dp0..\"
set "FILTER_PROJ=%ROOT%src\filter\isoflt.vcxproj"
set "DRIVER_PROJ=%ROOT%src\isotest\isotest.vcxproj"
set "HARNESS_SRC=%ROOT%tools\isotest\isotest.c"
set "HARNESS_OUT=%ROOT%tools\_build\isotest"
set "HARNESS_EXE=%HARNESS_OUT%\isotest.exe"
set "DRIVER_OUT=%ROOT%src\isotest\x64\%CFG%\isotest"
set "FILTER_OUT=%ROOT%src\filter\x64\%CFG%\isoflt"
set "FILTER_SYS=%FILTER_OUT%\isoflt.sys"
set "STAGED_FILTER_SYS=%DRIVER_OUT%\isoflt.sys"
echo =======================================================================
echo  DeckBtIsoTest Build: %CFG% x64
echo =======================================================================
if not exist "%HARNESS_OUT%\" mkdir "%HARNESS_OUT%"
if errorlevel 1 (
    echo [-] BUILD FAILED: cannot create harness output directory
    exit /b 1
)

rem 1. Build and test-sign the KMDF bus-interface filter driver via msbuild
echo [*] Building KMDF bus-interface filter driver (isoflt.sys)...
call "%EWDK%\BuildEnv\SetupBuildEnv.cmd" >nul

msbuild "%FILTER_PROJ%" /nologo /v:minimal /warnaserror /t:Build /p:Configuration=%CFG% /p:Platform=x64
if errorlevel 1 (
    echo.
    echo [-] BUILD FAILED: isoflt filter driver
    exit /b 1
)

if not exist "%FILTER_SYS%" (
    echo.
    echo [-] BUILD FAILED: isoflt.sys missing at %FILTER_SYS%
    exit /b 1
)

rem Stage isoflt.sys into the package output directory before driver packaging
if not exist "%DRIVER_OUT%\" mkdir "%DRIVER_OUT%"
copy /y "%FILTER_SYS%" "%STAGED_FILTER_SYS%" >nul
if errorlevel 1 (
    echo.
    echo [-] BUILD FAILED: cannot stage isoflt.sys to %DRIVER_OUT%
    exit /b 1
)

rem 2. Build and test-sign the UDE driver package via msbuild
echo.
echo [*] Building KMDF UDE driver package (isotest.sys + isoflt.sys + isotest.cat)...
msbuild "%DRIVER_PROJ%" /nologo /v:minimal /warnaserror /t:Build /p:Configuration=%CFG% /p:Platform=x64
if errorlevel 1 (
    echo.
    echo [-] BUILD FAILED: isotest driver
    exit /b 1
)

rem Ensure isoflt.sys is staged in the package directory
if not exist "%STAGED_FILTER_SYS%" (
    copy /y "%FILTER_SYS%" "%STAGED_FILTER_SYS%" >nul
    if errorlevel 1 (
        echo.
        echo [-] BUILD FAILED: cannot stage isoflt.sys to %DRIVER_OUT%
        exit /b 1
    )
)

rem 3. Verify filter binary and its signature in the staged package
echo.
echo [*] Verifying isoflt.sys binary and signature...
if not exist "%STAGED_FILTER_SYS%" (
    echo.
    echo [-] BUILD FAILED: staged filter binary missing at %STAGED_FILTER_SYS%
    exit /b 1
)

signtool.exe verify /pa "%STAGED_FILTER_SYS%" >nul 2>&1
if errorlevel 1 (
    signtool.exe verify /pa /c "%DRIVER_OUT%\isotest.cat" "%STAGED_FILTER_SYS%" >nul 2>&1
    if errorlevel 1 (
        echo.
        echo [-] BUILD FAILED: isoflt.sys signature is missing or invalid
        exit /b 1
    )
)
echo [+] isoflt.sys signature verified.

rem 4. Compile user-mode measurement harness
echo.
echo [*] Compiling user-mode measurement harness (isotest.exe)...
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

cl.exe /nologo /W4 /WX /O2 /DUNICODE /D_UNICODE /Fe:"%HARNESS_EXE%" /Fo:"%HARNESS_OUT%\\" "%HARNESS_SRC%" /link winusb.lib setupapi.lib
if errorlevel 1 (
    echo.
    echo [-] BUILD FAILED: isotest harness
    exit /b 1
)

echo.
echo [+] BUILD SUCCEEDED: driver and harness commands returned success
echo.
echo Driver Package Output (%DRIVER_OUT%):
dir /b "%DRIVER_OUT%" 2>nul
echo.
echo Harness Output:
dir /b "%HARNESS_EXE%" 2>nul
echo.
exit /b 0
