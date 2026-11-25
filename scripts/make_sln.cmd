@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  Generate the Visual Studio 2026 solution: build\vs2026\YCGrid.slnx (the VS 2026 solution format), and
rem  ycgrid.slnx at the repository root with the same projects (open either one)
rem    scripts\make_sln.cmd [/open] [/silent]
rem  /open: open it in Visual Studio right away
rem  Switch between Debug / RelWithDebInfo with the toolbar; the startup project is ycgrid.
rem  Only uses tools built into Windows and Visual Studio; no PowerShell.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "OPEN=0"
set "SILENT=0"
for %%A in (%*) do (
  if /I "%%~A"=="/open"   set "OPEN=1"
  if /I "%%~A"=="/silent" set "SILENT=1"
)

rem --- 1. Visual Studio ---------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  set "ERR=vswhere.exe not found. Install Visual Studio 2026 with the Desktop development with C++ workload."
  goto :fail
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
  set "ERR=No Visual Studio with the C++ toolset was found."
  goto :fail
)
echo [YCGrid] Visual Studio: %VSDIR%
if not defined VSCMD_VER (
  call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  if not "!errorlevel!"=="0" (
    set "ERR=Loading vcvars64.bat failed."
    goto :fail
  )
)
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  set "ERR=vcpkg not found (%VCPKG_ROOT%). Select the vcpkg package manager in the Visual Studio Installer, or set VCPKG_ROOT."
  goto :fail
)

rem --- 2. submodule -------------------------------------------------------------
pushd "%ROOT%"
if not exist "third_party\imgui\imgui.h" (
  echo [YCGrid] Initializing submodules...
  git submodule update --init --recursive
  if not "!errorlevel!"=="0" (
    set "ERR=git submodule update failed; make sure git is installed and GitHub is reachable."
    goto :fail_pop
  )
)

rem --- 3. Generate the solution ----------------------------------------------------------------
echo [YCGrid] cmake --preset vs2026
cmake --preset vs2026
if not "!errorlevel!"=="0" (
  set "ERR=CMake failed to generate the solution (if the message mentions deps.lock, a third-party version doesn't match; see third_party\README.md)."
  goto :fail_pop
)
popd

set "SLN=%ROOT%\build\vs2026\YCGrid.slnx"
if not exist "%SLN%" (
  set "ERR=%SLN% was not generated."
  goto :fail
)
rem the same solution at the root: its project paths point into build\vs2026
set "ROOTSLN=%ROOT%\ycgrid.slnx"
cmake -DIN="%SLN%" -DOUT="%ROOTSLN%" -DPREFIX=build/vs2026/ -P "%ROOT%\cmake\YcRootSolution.cmake"
if not "!errorlevel!"=="0" (
  set "ERR=Writing %ROOTSLN% failed."
  goto :fail
)
echo [YCGrid] Solution generated: %ROOTSLN% (and %SLN%)
if "%OPEN%"=="1" start "" "%VSDIR%\Common7\IDE\devenv.exe" "%ROOTSLN%"
endlocal & exit /b 0

:fail_pop
popd
:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
