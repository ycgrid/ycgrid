@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid one-step build
rem    scripts\build.cmd [release|debug|clang] [/silent]
rem  1) find Visual Studio with vswhere  2) load vcvars64  3) check the submodules
rem  4) cmake --preset (with the deps.lock version checks)  5) cmake --build
rem  Only uses tools built into Windows and Visual Studio; no PowerShell.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
rem error codes are always checked with if not "!errorlevel!"=="0": the linker may return -1, and if errorlevel 1 is false for negative values.

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "PRESET=msvc-release"
set "SILENT=0"
for %%A in (%*) do (
  if /I "%%~A"=="release" set "PRESET=msvc-release"
  if /I "%%~A"=="debug"   set "PRESET=msvc-debug"
  if /I "%%~A"=="clang"   set "PRESET=clang-cl-release"
  if /I "%%~A"=="/silent" set "SILENT=1"
)

echo [YCGrid] Build configuration: %PRESET%

rem --- 1. Visual Studio ---------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  set "ERR=vswhere.exe not found. Install Visual Studio 2026 with the Desktop development with C++ workload."
  goto :fail
)
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR (
  set "ERR=No Visual Studio with the C++ toolset was found. Select Desktop development with C++ in the Visual Studio Installer."
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
if not exist "third_party\implot\implot.h" (
  git submodule update --init --recursive
  if not "!errorlevel!"=="0" (
    set "ERR=git submodule update failed."
    goto :fail_pop
  )
)

rem --- 3. configure + build -----------------------------------------------------
echo [YCGrid] cmake --preset %PRESET%
cmake --preset %PRESET%
if not "!errorlevel!"=="0" (
  set "ERR=CMake configure failed (if the message mentions deps.lock, a third-party version doesn't match; see third_party\README.md)."
  goto :fail_pop
)
echo [YCGrid] cmake --build --preset %PRESET%
cmake --build --preset %PRESET%
if not "!errorlevel!"=="0" (
  set "ERR=Compilation failed."
  goto :fail_pop
)

rem --- 4. Unit tests ----------------------------------------------------------------
if exist "build\%PRESET%\tests\yc_unit_tests.exe" (
  echo [YCGrid] Unit tests
  "build\%PRESET%\tests\yc_unit_tests.exe"
  if not "!errorlevel!"=="0" (
    set "ERR=Unit tests failed."
    goto :fail_pop
  )
)
popd

echo [YCGrid] Build finished: build\%PRESET%\
endlocal & exit /b 0

:fail_pop
popd
:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
