@echo off
rem Build WinTools.
rem
rem Must run from a plain shell: this script enters the MSVC developer environment itself,
rem because Ninja + cl.exe needs vcvars applied first.
rem
rem Why the extra SDK step at the end: on this machine vcvars64.bat reports success but leaves
rem rc.exe and mt.exe off PATH, so CMake configures "rc=rc / mt=CMAKE_MT-NOTFOUND" and every
rem link step dies with "RC Pass 1 ... no such file or directory". Adding the SDK bin folder
rem explicitly costs one directory scan and removes the whole class of failure.
rem
rem Deliberately written with goto instead of parenthesised blocks: %ProgramFiles(x86)% contains
rem a closing parenthesis, which truncates an if-block early and silently skips its body.
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "ROOT=%SCRIPT_DIR%.."
set "PF86=%ProgramFiles(x86)%"
set "PF=%ProgramFiles%"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=windows-x64-debug"

if defined VCINSTALLDIR goto :have_vs

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :no_toolset

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :no_vcvars

:have_vs

if not defined VSPATH (
    set "VSPATH=%VCINSTALLDIR%..\.."
)

rem ---- Locate the Windows SDK independently of vcvars --------------------------------
rem vcvars64.bat resolves the SDK through the registry. When registry access is unavailable
rem (sandboxed shell, locked-down image) it reports success but leaves WindowsSdkDir empty,
rem so INCLUDE/LIB have no ucrt or um folders and the link step dies with
rem "LNK1104: cannot open kernel32.lib". Resolving by directory scan costs one listing.

set "SDKBASE=%PF86%\Windows Kits\10"
set "SDKVER="
if exist "%SDKBASE%\Include" for /d %%v in ("%SDKBASE%\Include\10.*") do set "SDKVER=%%~nxv"
if not defined SDKVER goto :no_sdk

set "SDKINC=%SDKBASE%\Include\%SDKVER%"
set "SDKLIB=%SDKBASE%\Lib\%SDKVER%"
set "SDKBIN=%SDKBASE%\bin\%SDKVER%\x64"

if not exist "%SDKINC%\um\windows.h" goto :no_sdk
if not exist "%SDKLIB%\um\x64\kernel32.lib" goto :no_sdk

rem Prepend, so a half-initialised environment cannot shadow these.
set "INCLUDE=%SDKINC%\ucrt;%SDKINC%\um;%SDKINC%\shared;%SDKINC%\winrt;%INCLUDE%"
set "LIB=%SDKLIB%\ucrt\x64;%SDKLIB%\um\x64;%LIB%"
set "PATH=%SDKBIN%;%PATH%"

echo [info] Windows SDK %SDKVER% wired into INCLUDE/LIB/PATH

rem ---- CMake ---------------------------------------------------------------------------
where cmake >nul 2>&1
if not errorlevel 1 goto :have_cmake
if defined VSPATH set "PATH=!VSPATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;!PATH!"
where cmake >nul 2>&1
if errorlevel 1 goto :no_cmake

:have_cmake

cd /d "%ROOT%"

rem "if errorlevel 1" is not enough here: cmake exits with -1 (0xFFFFFFFF) when the build tool
rem fails, and the numeric comparison never sees that as >= 1. The whole script then fell through
rem to "done:" and handed back the *previous* exe - which looks exactly like a successful build
rem until the stale binary is run. Compare against 0 explicitly instead.
echo [1/2] configure %PRESET%
cmake --preset %PRESET%
if not "!ERRORLEVEL!"=="0" goto :fail

echo [2/2] build %PRESET%
cmake --build --preset %PRESET%
if not "!ERRORLEVEL!"=="0" goto :fail

rem Preset names and binary dirs differ (CMakePresets uses build/debug and build/release),
rem so map explicitly rather than string-munging the preset name.
set "OUTDIR=debug"
if "%PRESET%"=="windows-x64-release" set "OUTDIR=release"

echo.
echo done: %ROOT%\build\%OUTDIR%\bin\WinTools.exe
exit /b 0

:no_vswhere
echo [error] vswhere.exe not found. Install Visual Studio Build Tools with the
echo         "Desktop development with C++" workload.
exit /b 1

:no_toolset
echo [error] No MSVC x64 toolset found by vswhere.
exit /b 1

:no_vcvars
echo [error] vcvars64.bat failed.
exit /b 1

:no_sdk
echo [error] rc.exe not found: no usable Windows 10/11 SDK installation.
exit /b 1

:no_cmake
echo [error] cmake not found, neither in PATH nor inside the Visual Studio installation.
exit /b 1

:fail
echo [error] build failed.
exit /b 1
