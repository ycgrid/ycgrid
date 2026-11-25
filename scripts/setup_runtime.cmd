@echo off
chcp 65001 >nul
rem SPDX-License-Identifier: Apache-2.0
rem ============================================================================
rem  Install YCGrid's own portable Python runtime (uv + CPython, versions in third_party\deps.lock)
rem    scripts\setup_runtime.cmd [target folder] [/silent]
rem  Default target folder: build\runtime (for development; ycgrid.exe searches parent folders for runtime\python)
rem  Never writes the registry, never changes PATH, never touches the system Python; caches go to _work in the target folder.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "DEST=%ROOT%\build\runtime"
set "SILENT=0"
for %%A in (%*) do (
  if /I "%%~A"=="/silent" (set "SILENT=1") else (set "DEST=%%~fA")
)
set "LOCK=%ROOT%\third_party\deps.lock"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"uv.url=" "%LOCK%"') do set "UV_URL=%%B"
for /f "tokens=1,* delims==" %%A in ('findstr /b /c:"python.version=" "%LOCK%"') do set "PY_VER=%%B"
if not defined UV_URL (
  set "ERR=deps.lock has no uv.url."
  goto :fail
)
if exist "%DEST%\python\python.exe" (
  echo [YCGrid] Already installed: %DEST%\python
  "%DEST%\python\python.exe" -E -s -c "import sys; print('[YCGrid] Python', sys.version.split()[0])"
  call :packages
  if not "!errorlevel!"=="0" goto :fail
  endlocal & exit /b 0
)
set "WORK=%DEST%\_work"
if not exist "%WORK%" mkdir "%WORK%"
if not exist "%DEST%\uv.exe" (
  echo [YCGrid] Downloading uv: %UV_URL%
  "%SystemRoot%\System32\curl.exe" -sSL --fail -o "%WORK%\uv.zip" "%UV_URL%"
  if not "!errorlevel!"=="0" (
    set "ERR=Downloading uv failed; check the network connection."
    goto :fail
  )
   "%SystemRoot%\System32\tar.exe" -xf "%WORK%\uv.zip" -C "%DEST%"
  if not exist "%DEST%\uv.exe" (
    set "ERR=Extracting uv failed."
    goto :fail
  )
)
set "UV_CACHE_DIR=%WORK%\cache"
set "UV_PYTHON_INSTALL_DIR=%WORK%\python"
echo [YCGrid] Installing CPython %PY_VER%
"%DEST%\uv.exe" python install %PY_VER% --install-dir "%WORK%\python" --no-bin --no-registry
if not "!errorlevel!"=="0" (
  set "ERR=uv python install failed."
  goto :fail
)
set "PY_SRC="
for /d %%D in ("%WORK%\python\cpython-%PY_VER%*") do set "PY_SRC=%%~fD"
if not defined PY_SRC (
  set "ERR=The installed CPython was not found."
  goto :fail
)
robocopy "%PY_SRC%" "%DEST%\python" /E /NFL /NDL /NJH /NJS /NP >nul
if exist "%DEST%\python\Lib\EXTERNALLY-MANAGED" del /q "%DEST%\python\Lib\EXTERNALLY-MANAGED"
> "%DEST%\python\.yc-python.json" echo {"version": "%PY_VER%", "source": "python-build-standalone via uv", "abi": "cp312"}
"%DEST%\python\python.exe" -E -s -c "import sys; print('[YCGrid] Python', sys.version.split()[0], sys.prefix)"
call :packages
if not "!errorlevel!"=="0" goto :fail
endlocal & exit /b 0

rem the research packages (python\requirements-runtime.txt): numpy, pandas...
:packages
set "REQ=%ROOT%\python\requirements-runtime.txt"
if not exist "%REQ%" exit /b 0
if not exist "%DEST%\uv.exe" exit /b 0
"%DEST%\python\python.exe" -E -s -c "import numpy, pandas" 2>nul
if "!errorlevel!"=="0" exit /b 0
echo [YCGrid] Installing the research packages (numpy, pandas)
set "UV_CACHE_DIR=%DEST%\_work\cache"
"%DEST%\uv.exe" pip install --python "%DEST%\python\python.exe" -r "%REQ%"
if not "!errorlevel!"=="0" (
  set "ERR=Installing the research packages failed; check the network connection."
  exit /b 1
)
exit /b 0

:fail
echo.
echo [YCGrid] Failed: %ERR%
if "%SILENT%"=="0" pause
endlocal & exit /b 1
