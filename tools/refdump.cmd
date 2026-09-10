@echo off
rem Regenerate reference\VIRTUAL-HCI-REFERENCE.txt from the production sources.
setlocal
set "EWDK=C:\EWDK"
set "MSVC_ROOT=%EWDK%\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
set "SDK=%EWDK%\Program Files\Windows Kits\10"
set "SDKVER=10.0.26100.0"
for /d %%d in ("%MSVC_ROOT%\*") do set "MSVC=%%d"
if not defined MSVC ( echo ERROR: no MSVC toolset under "%MSVC_ROOT%" & exit /b 1 )
set "PATH=%MSVC%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKVER%\ucrt\x64;%SDK%\Lib\%SDKVER%\um\x64"
set "HERE=%~dp0"
set "OUT=%HERE%_build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%HERE%..\reference" mkdir "%HERE%..\reference"
cl.exe /nologo /W4 /WX /Fe:"%OUT%\refdump.exe" /Fo:"%OUT%\\" ^
   "%HERE%refdump.c" "%HERE%..\src\common\usb_descriptors.c" "%HERE%..\src\driver\hci_stub.c"
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
"%OUT%\refdump.exe" > "%HERE%..\reference\VIRTUAL-HCI-REFERENCE.txt"
echo Wrote reference\VIRTUAL-HCI-REFERENCE.txt
exit /b 0
