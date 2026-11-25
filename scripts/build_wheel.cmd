@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  YCGrid wheel: pip install ycgrid for people who use their own Python
rem    scripts\build_wheel.cmd [release|debug]
rem  Packs python\yc with ycgrid.exe (from dist_release or dist_debug) into
rem  build\wheel\ycgrid-<version>-py3-none-win_amd64.whl. Run scripts\build.cmd first.
rem  Uses the bundled Python (runtime\python), never the system Python.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "DIST=%ROOT%\dist_release"
for %%A in (%*) do (
  if /I "%%~A"=="debug" set "DIST=%ROOT%\dist_debug"
)
set "PY=%DIST%\runtime\python\python.exe"
if not exist "%PY%" set "PY=%ROOT%\build\runtime\python\python.exe"
if not exist "%PY%" (
  set "ERR=The bundled Python wasn't found. Run scripts\setup_runtime.cmd or scripts\build.cmd first."
  goto :fail
)
if not exist "%DIST%\ycgrid.exe" (
  set "ERR=%DIST%\ycgrid.exe doesn't exist. Run scripts\build.cmd first."
  goto :fail
)
"%PY%" -E -s "%ROOT%\tools\make_wheel.py" "%DIST%" "%ROOT%\build\wheel"
if not "!errorlevel!"=="0" (
  set "ERR=Building the wheel failed."
  goto :fail
)
set /p VER=<"%ROOT%\VERSION"
set "WHL=%ROOT%\build\wheel\ycgrid-%VER%-py3-none-win_amd64.whl"
if not exist "%WHL%" (
  set "ERR=%WHL% wasn't made: VERSION and python\yc\__init__.py name different versions."
  goto :fail
)
rem its SHA-256 goes next to it, for the release page
set "HASH="
for /f "skip=1 delims=" %%H in ('%SystemRoot%\System32\certutil.exe -hashfile "%WHL%" SHA256') do if not defined HASH set "HASH=%%H"
if not defined HASH (
  set "ERR=certutil couldn't hash the wheel."
  goto :fail
)
set "HASH=!HASH: =!"
> "%WHL%.sha256" <nul set /p "=!HASH!  ycgrid-%VER%-py3-none-win_amd64.whl"
echo [YCGrid] SHA-256 !HASH!
echo [YCGrid] Install it with: pip install build\wheel\ycgrid-%VER%-py3-none-win_amd64.whl
exit /b 0

:fail
echo [YCGrid] Failed: !ERR!
exit /b 1
