@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  Double-click to build YCGrid and lay out the new version, ready to install on this PC
rem    build_release.cmd           build (release), run the unit tests, stage dist_release\ and open it in Explorer:
rem                                then double-click install.cmd there to install the new version
rem    build_release.cmd /silent   no Explorer window and no pause (scripts)
rem  Needs what setup.cmd installs (Git, Visual Studio with the C++ tools and vcpkg): run setup.cmd once on a new PC.
rem  The portable Python (build\runtime) is set up here when it is missing.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
rem error codes are always checked with if not "!errorlevel!"=="0": the linker may return -1.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "SILENT=0"
for %%A in (%*) do if /I "%%~A"=="/silent" set "SILENT=1"
set "DIST=%ROOT%\dist_release"

if not exist "%ROOT%\build\runtime\python\python.exe" (
  echo [YCGrid] Setting up the portable Python first
  call "%ROOT%\scripts\setup_runtime.cmd" /silent
  if not "!errorlevel!"=="0" (
    set "ERR=Setting up the portable Python failed; the lines above say why."
    goto :fail
  )
)

call "%ROOT%\scripts\build.cmd" release /silent
if not "!errorlevel!"=="0" (
  set "ERR=The build failed; the lines above say why. If dist_release\ycgrid.exe is running, close it and try again."
  goto :fail
)
for %%F in (ycgrid.exe install.cmd runtime\python\python.exe examples\demo\demo.ycg) do (
  if not exist "%DIST%\%%F" (
    set "ERR=%%F is missing from %DIST% after the build."
    goto :fail
  )
)

echo.
echo [YCGrid] Done: the new version is in %DIST%
echo [YCGrid] To install it on this PC, close YCGrid and double-click install.cmd in that folder.
if "%SILENT%"=="0" (
  start "" "%SystemRoot%\explorer.exe" "%DIST%"
  pause
)
endlocal & exit /b 0

:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
