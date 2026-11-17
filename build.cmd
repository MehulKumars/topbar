@echo off
rem Builds build\TopBar.exe with the Visual Studio C++ toolchain (2019 or newer).
rem Usage: build.cmd            release build
rem        build.cmd debug      debug build with symbols
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:havecl

if not exist build mkdir build

set "CFLAGS=/nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /GS /guard:cf /MP"
set "LFLAGS=/OPT:REF /OPT:ICF"
if /i "%~1"=="debug" goto :debug
set "CFLAGS=%CFLAGS% /O2 /MT /DNDEBUG"
goto :build
:debug
set "CFLAGS=%CFLAGS% /Od /Zi /MTd /D_DEBUG"
set "LFLAGS=/DEBUG"
:build

rc /nologo /i res /fo build\topbar.res res\topbar.rc || exit /b 1

cl %CFLAGS% /Fobuild\ /Fdbuild\ src\*.cpp build\topbar.res /Fe:build\TopBar.exe ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /GUARD:CF %LFLAGS% ^
  user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib dwmapi.lib ^
  d2d1.lib dwrite.lib iphlpapi.lib wlanapi.lib bthprops.lib setupapi.lib pdh.lib shcore.lib ^
  wtsapi32.lib powrprof.lib propsys.lib dxgi.lib windowscodecs.lib delayimp.lib ^
  /DELAYLOAD:wlanapi.dll /DELAYLOAD:bthprops.cpl /DELAYLOAD:pdh.dll /DELAYLOAD:powrprof.dll || exit /b 1

echo.
echo Built build\TopBar.exe
exit /b 0

:novs
echo Visual Studio with the "Desktop development with C++" workload was not found.
exit /b 1
