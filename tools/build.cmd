@echo off
rem Build the DeckBtUsb driver package with the extracted EWDK. No elevation, no installation.
rem   tools\build.cmd            -> Release
rem   tools\build.cmd Debug      -> Debug
setlocal

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"

set "EWDK=C:\EWDK"
if not exist "%EWDK%\BuildEnv\SetupBuildEnv.cmd" (
  echo ERROR: EWDK not found at %EWDK%
  exit /b 1
)

set "PROJ=%~dp0..\src\driver\deckbtusb.vcxproj"
set "PROJ2=%~dp0..\src\filter\deckbtflt.vcxproj"

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

echo.
echo Build output:
dir /b "%~dp0..\src\driver\x64\%CFG%\deckbtusb" 2>nul
dir /b "%~dp0..\src\filter\x64\%CFG%\deckbtflt" 2>nul
exit /b 0
